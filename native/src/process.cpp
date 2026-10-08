#include <amp/process.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>

namespace amp {
namespace {
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void close() { if (value) CloseHandle(value); value = nullptr; }
};
std::wstring environment_value(const wchar_t* name) {
    auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring value(size, L'\0');
    auto count = GetEnvironmentVariableW(name, value.data(), size);
    if (!count || count >= size) return {};
    value.resize(count); return value;
}
std::filesystem::path executable_path(const std::filesystem::path& value) {
    if (value.extension() != L".exe") throw std::runtime_error("Child executable must be an EXE");
    if (value.is_absolute() && std::filesystem::is_regular_file(value)) return value;
    if (value.has_parent_path()) throw std::runtime_error("Child executable must use an absolute path");
    std::array<wchar_t, 32768> found{};
    auto count = SearchPathW(nullptr, value.c_str(), nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (!count || count >= found.size()) throw std::runtime_error("Required child executable is missing");
    return std::filesystem::path(found.data());
}
}
namespace process_detail {
std::wstring quote_argument(std::wstring_view value) {
    if (value.find(L'\0') != value.npos) throw std::runtime_error("Invalid child argument");
    std::wstring result = L"\""; size_t slashes{};
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { result.append(slashes * 2 + 1, L'\\'); result += c; }
        else { result.append(slashes, L'\\'); result += c; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
std::vector<wchar_t> child_environment(bool credential_mode) {
    std::map<std::wstring, std::wstring> variables;
    for (auto name : {L"PATH", L"SYSTEMROOT", L"WINDIR", L"TEMP", L"TMP"}) {
        auto value = environment_value(name); if (!value.empty()) variables[name] = value;
    }
    if (credential_mode) {
        for (auto name : {L"USERPROFILE", L"HOME", L"APPDATA", L"LOCALAPPDATA", L"PROGRAMFILES", L"PROGRAMFILES(X86)", L"PROGRAMDATA"}) {
            auto value = environment_value(name); if (!value.empty()) variables[name] = value;
        }
        variables[L"GIT_TERMINAL_PROMPT"] = L"0";
        variables[L"GCM_INTERACTIVE"] = L"Never";
        variables[L"GCM_TRACE"] = L"0";
        variables[L"GCM_TRACE_SECRETS"] = L"0";
        variables[L"GCM_TRACE_MSAUTH"] = L"0";
        variables[L"GCM_DEBUG"] = L"0";
        variables[L"GIT_ASKPASS"] = L"";
        variables[L"SSH_ASKPASS"] = L"";
    }
    std::vector<wchar_t> result;
    for (const auto& [key, value] : variables) {
        auto entry = key + L"=" + value;
        result.insert(result.end(), entry.begin(), entry.end()); result.push_back(L'\0');
    }
    result.push_back(L'\0'); if (result.size() == 1) result.push_back(L'\0'); return result;
}
}
ProcessResult run_process(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
                          std::string_view input, std::chrono::milliseconds timeout, HANDLE stop, bool credential_mode) {
    if (input.size() > 4096 || timeout.count() <= 0 || timeout > std::chrono::seconds(60)) throw std::runtime_error("Invalid child limits");
    auto cancelled = [&] { return stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; };
    if (cancelled()) throw std::runtime_error("Child process cancelled");
    auto path = executable_path(executable);
    auto command = process_detail::quote_argument(path.wstring());
    for (const auto& argument : arguments) { command += L' '; command += process_detail::quote_argument(argument); }
    if (command.size() >= 32767) throw std::runtime_error("Child command is too long");
    auto environment = process_detail::child_environment(credential_mode);
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle in_read, in_write, out_read, out_write, job;
    if (!CreatePipe(&in_read.value, &in_write.value, &security, 8192)
        || !CreatePipe(&out_read.value, &out_write.value, &security, 0)
        || !SetHandleInformation(in_write.value, HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(out_read.value, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Child pipes unavailable");
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Child cancellation unavailable");
    SIZE_T attribute_size{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> storage(attribute_size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) throw std::runtime_error("Child handle isolation unavailable");
    struct AttributeGuard { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeGuard() { DeleteProcThreadAttributeList(value); } } guard{attributes};
    HANDLE inherited[]{in_read.value, out_write.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        throw std::runtime_error("Child handle isolation unavailable");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = in_read.value; startup.StartupInfo.hStdOutput = out_write.value; startup.StartupInfo.hStdError = out_write.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment.data(), path.parent_path().c_str(), &startup.StartupInfo, &process)) throw std::runtime_error("Child process could not start");
    Handle process_handle{process.hProcess}, thread_handle{process.hThread};
    if (!AssignProcessToJobObject(job.value, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000);
        throw std::runtime_error("Child process isolation failed");
    }
    // Every exit path closes the job and terminates descendants. Join before
    // callers remove the conversion directory, including timeout/cancellation.
    struct ProcessGuard {
        HANDLE job, process;
        ~ProcessGuard() {
            TerminateJobObject(job, 1); WaitForSingleObject(process, 5000);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
            while (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)
                   && accounting.ActiveProcesses && std::chrono::steady_clock::now() < deadline) Sleep(10);
        }
    } process_guard{job.value, process.hProcess};
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) throw std::runtime_error("Child process could not resume");
    thread_handle.close(); in_read.close(); out_write.close();
    if (!input.empty()) {
        DWORD written{};
        if (!WriteFile(in_write.value, input.data(), static_cast<DWORD>(input.size()), &written, nullptr) || written != input.size())
            throw std::runtime_error("Child input failed");
    }
    in_write.close();
    auto deadline = std::chrono::steady_clock::now() + timeout;
    ProcessResult result;
    std::array<char, 8192> buffer{};
    for (;;) {
        if (cancelled()) throw std::runtime_error("Child process cancelled");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Child process timed out");
        DWORD available{};
        if (PeekNamedPipe(out_read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD count{};
            if (!ReadFile(out_read.value, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &count, nullptr))
                throw std::runtime_error("Child output failed");
            if (count > 1024 * 1024 - result.output.size()) throw std::runtime_error("Child output is too large");
            result.output.append(buffer.data(), count); continue;
        }
        if (WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0) {
            // Stop any helper descendants before finishing the bounded pipe drain.
            TerminateJobObject(job.value, 1);
            if (!GetExitCodeProcess(process.hProcess, &result.exit_code)) throw std::runtime_error("Child status unavailable");
            if (PeekNamedPipe(out_read.value, nullptr, 0, nullptr, &available, nullptr) && available) continue;
            return result;
        }
    }
}
}

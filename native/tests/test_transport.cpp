#include <amp/http.hpp>
#include <amp/process.hpp>
#include <amp/settings.hpp>
#include <nlohmann/json.hpp>
#include <cassert>
#include <chrono>
#include <thread>

void test_transport() {
    using namespace amp;
    using namespace std::chrono;
    assert(http_detail::permitted_url("https://music.apple.com/us/album/name/123"));
    assert(http_detail::permitted_url("https://is1-ssl.mzstatic.com/image/thumb/cover.jpg"));
    assert(http_detail::permitted_url("https://api.github.com/repos/user/repo", true));
    for (auto url : {"http://api.github.com/", "https://api.github.com.evil.com/", "https://evil.mzstatic.com.evil/",
        "https://api.github.com@evil.com/", "https://api.github.com:444/", "https://api.github.com/\\evil",
        "https://api.github.com/#token", "https://api.github.com/\r\nInjected: header", "https://127.0.0.1/"})
        assert(!http_detail::permitted_url(url));
    assert(!http_detail::permitted_url("https://raw.githubusercontent.com/user/repo/file.webp", true));
    bool rejected{};
    try { http_request("https://raw.githubusercontent.com/user/repo/file.webp", 1, "GET", "", "fake-token"); }
    catch (...) { rejected = true; } assert(rejected);
    HANDLE stop = CreateEventW(nullptr, TRUE, TRUE, nullptr); assert(stop);
    rejected = false;
    try { http_get("https://music.apple.com/", 1, stop); } catch (...) { rejected = true; } assert(rejected);
    std::wstring executable(32768, L'\0');
    auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())); assert(size);
    executable.resize(size);
    const auto secrets_before = process_detail::child_environment(false);
    assert(SetEnvironmentVariableW(L"AMP_FAKE_SECRET", L"do-not-inherit"));
    try {
        const std::vector<std::wstring> arguments{L"--child-echo", L"plain", L"", L"spaces and quotes \"inside\"",
            L"C:\\folder with spaces\\", L"Unicode \u00e9 \U0001f3b5", L"slashes\\\\\"quote"};
        auto normal = run_process(executable, arguments, "test input\n", seconds(5));
        assert(normal.exit_code == 0);
        auto data = nlohmann::json::parse(normal.output);
        assert(data["secrets"] == false && data["input"] == "test input\n" && data["interactive"] == false);
        for (size_t i = 1; i < arguments.size(); ++i) assert(data["arguments"][i - 1] == utf8(arguments[i]));
        auto credential = run_process(executable, {L"--child-echo"}, "public input", seconds(5), nullptr, true);
        data = nlohmann::json::parse(credential.output);
        assert(credential.exit_code == 0 && data["secrets"] == false && data["interactive"] == true);
        auto begin = steady_clock::now(); rejected = false;
        try { run_process(executable, {L"--child-wait"}, "", seconds(5), stop); } catch (...) { rejected = true; }
        assert(rejected && steady_clock::now() - begin < seconds(1));
        ResetEvent(stop);
        std::thread cancel([stop] { Sleep(120); SetEvent(stop); });
        begin = steady_clock::now(); rejected = false;
        try { run_process(executable, {L"--child-wait"}, "", seconds(5), stop); } catch (...) { rejected = true; }
        cancel.join(); assert(rejected && steady_clock::now() - begin < seconds(2));
        begin = steady_clock::now(); rejected = false;
        try { run_process(executable, {L"--child-wait"}, "", milliseconds(120)); } catch (...) { rejected = true; }
        assert(rejected && steady_clock::now() - begin < seconds(2));
        rejected = false;
        try { run_process(executable, {L"--child-overflow"}, "", seconds(5)); } catch (...) { rejected = true; }
        assert(rejected);
    } catch (...) { SetEnvironmentVariableW(L"AMP_FAKE_SECRET", nullptr); CloseHandle(stop); throw; }
    SetEnvironmentVariableW(L"AMP_FAKE_SECRET", nullptr); CloseHandle(stop);
    assert(process_detail::child_environment(false) == secrets_before);
}

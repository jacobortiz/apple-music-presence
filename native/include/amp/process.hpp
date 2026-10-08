#pragma once
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace amp {
struct ProcessResult { DWORD exit_code{}; std::string output; };
ProcessResult run_process(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
                          std::string_view input, std::chrono::milliseconds timeout, HANDLE stop = nullptr, bool credential_mode = false);
namespace process_detail {
std::wstring quote_argument(std::wstring_view value);
std::vector<wchar_t> child_environment(bool credential_mode);
}
}

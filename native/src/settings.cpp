#include <amp/settings.hpp>
#include <amp/artwork_host.hpp>
#include <Windows.h>
#include <ShlObj.h>
#include <chrono>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <vector>

namespace amp {
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Windows text");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
double unix_time() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::filesystem::path data_directory() {
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)))
        throw std::runtime_error("Windows could not locate your preferences folder");
    std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path / L"AppleMusicPresence";
}
std::optional<nlohmann::json> read_json(const std::filesystem::path& path, size_t limit) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        std::string text(limit + 1, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(input.gcount()));
        if (text.size() > limit) return {};
        auto data = nlohmann::json::parse(text, [](int depth, auto, auto&) {
            if (depth > 64) throw std::runtime_error("JSON nesting limit");
            return true;
        });
        if (data.is_object()) return data;
    } catch (...) { /* Invalid optional files use defaults; never log contents. */ }
    return {};
}
Settings load_settings(const std::filesystem::path& directory) {
    Settings result;
    // Import public/non-secret Python preferences once; never overwrite them.
    auto path = directory / L"native_settings.json";
    auto data = read_json(path, 16 * 1024);
    if (!std::filesystem::exists(path)) data = read_json(directory / L"settings.json", 16 * 1024);
    if (!data) return result;
    auto text = [&](const char* key, const std::string& fallback = "") {
        auto item = data->find(key);
        return item != data->end() && item->is_string() ? item->get<std::string>() : fallback;
    };
    auto flag = [&](const char* key) { return data->contains(key) && (*data)[key].is_boolean() && (*data)[key].get<bool>(); };
    result.client_id = text("client_id"); result.country = text("country", "US");
    result.source_id = text("source_id"); result.artwork_repository = text("artwork_repository");
    result.artwork = flag("artwork"); result.motion_artwork = flag("motion_artwork");
    result.start_with_windows = flag("start_with_windows");
    return result;
}
void validate_settings(const Settings& settings) {
    if (!std::regex_match(settings.client_id, std::regex("[0-9]{17,20}")))
        throw std::runtime_error("Enter the 17-20 digit Discord Application ID, not a token.");
    if (!std::regex_match(settings.country, std::regex("[A-Z]{2}")))
        throw std::runtime_error("Enter a two-letter Apple storefront, such as US or GB.");
    if (!settings.artwork_repository.empty() && !host_detail::valid_repository(settings.artwork_repository))
        throw std::runtime_error("Enter the public artwork repository as owner/repository, without a URL or token.");
}
void save_settings(const Settings& settings, const std::filesystem::path& directory) {
    validate_settings(settings);
    std::filesystem::create_directories(directory);
    nlohmann::json data{{"client_id", settings.client_id}, {"country", settings.country}, {"source_id", settings.source_id},
        {"artwork", settings.artwork}, {"motion_artwork", settings.motion_artwork},
        {"artwork_repository", settings.artwork_repository}, {"start_with_windows", settings.start_with_windows}};
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(directory.c_str(), L"amp", 0, temporary)) throw std::runtime_error("Could not save preferences");
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << data.dump(2) << '\n'; file.close();
        if (!file || !MoveFileExW(temporary, (directory / L"native_settings.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not save preferences");
    } catch (...) { DeleteFileW(temporary); throw; }
}
}

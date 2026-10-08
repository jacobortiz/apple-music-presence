#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace amp {
struct Settings {
    std::string client_id, country{"US"}, source_id;
    bool artwork{}, motion_artwork{};
    std::string artwork_repository;
    bool start_with_windows{};
};
std::filesystem::path data_directory();
Settings load_settings(const std::filesystem::path& directory);
void save_settings(const Settings&, const std::filesystem::path& directory);
void validate_settings(const Settings&);
std::optional<nlohmann::json> read_json(const std::filesystem::path&, size_t limit);
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
double unix_time();
}

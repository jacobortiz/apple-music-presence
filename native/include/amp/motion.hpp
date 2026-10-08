#pragma once

#include <Windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amp {
inline constexpr std::string_view motion_encoding_profile = "webp-768-q85-lanczos-v2";
inline constexpr std::size_t max_motion_image_bytes = 8 * 1024 * 1024;

struct PreparedMotion { std::string stream, webp; };

std::optional<std::string> discover_motion(std::string_view verified_page,
                                         std::string_view album, HANDLE stop = nullptr);
// An empty FFmpeg path resolves the optional converter beside the app or on PATH.
std::string convert_motion(std::string_view stream, const std::filesystem::path& ffmpeg,
                           const std::filesystem::path& temp_parent, HANDLE stop = nullptr);
std::optional<PreparedMotion> prepare_motion(std::string_view verified_page,
                                            std::string_view album,
                                            const std::filesystem::path& ffmpeg,
                                            const std::filesystem::path& temp_parent,
                                            HANDLE stop = nullptr);
std::filesystem::path ffmpeg_executable(const std::filesystem::path& app_directory);

namespace motion_detail {
struct Playlist {
    std::vector<std::pair<std::string, std::string>> media;
    std::string local;
};
bool apple_stream(std::string_view value);
std::optional<std::string> album_page(std::string_view value);
std::optional<std::string> album_redirect(std::string_view source, std::string_view location);
std::optional<std::string> find_motion(std::string_view html, std::string_view album_id,
                                      std::string_view album);
std::vector<std::string> rendition_urls(std::string_view master, std::string_view stream);
Playlist rewrite_playlist(std::string_view playlist, std::string_view variant);
bool animated_webp(std::string_view content);
}
}

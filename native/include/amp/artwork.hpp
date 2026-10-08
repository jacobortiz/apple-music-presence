#pragma once

#include "amp/model.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace amp {
struct Settings;

// Called by the dedicated artwork worker, never by media polling or the UI.
class ArtworkResolver {
public:
    ArtworkResolver(const Settings& settings, std::filesystem::path data_dir);
    ~ArtworkResolver();
    ArtworkResolver(const ArtworkResolver&) = delete;
    ArtworkResolver& operator=(const ArtworkResolver&) = delete;
    std::optional<Artwork> resolve(const Track& track);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pure validation functions also used by the offline regression tests.
namespace artwork_detail {
std::string normalize(std::string_view value);
std::optional<std::string> thumbnail(std::string_view value);
std::optional<std::string> album_key(std::string_view page);
std::optional<Artwork> catalog_match(std::string_view payload, const Track& track);
std::optional<Artwork> mapped_cover(std::string_view payload, const Track& track);
std::optional<Artwork> cached_motion(std::string_view payload, std::string_view repository,
                                    std::string_view verified_page, double now);
}
}

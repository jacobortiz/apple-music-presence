#pragma once
#include "amp/http.hpp"
#include "amp/model.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace amp {
using CatalogHttpGetter = std::function<HttpResponse(std::string_view, std::size_t, HANDLE)>;

namespace catalog_detail {
std::vector<Artwork> song_candidates(std::string_view json, const Track& track);
std::vector<Artwork> album_candidates(std::string_view json, std::string_view artist, std::string_view album);
std::optional<Artwork> known_cover(const std::vector<Artwork>& candidates);
std::optional<std::string> normalized_exif(std::string_view bytes);
std::optional<std::string> comparable_jpeg(std::string_view bytes);
std::optional<std::string> apple_album_page(std::string_view url);
std::optional<std::string> album_redirect(std::string_view page, std::string_view location);
std::vector<std::string> candidate_pages(std::string_view html, std::string_view title);
std::optional<Artwork> album_cover(std::string_view html, std::string_view page, const Track& track);
}

// Fetch at most four public Apple images. Only an exact display-byte match,
// after a validated EXIF UserComment normalization, permits a shared cover.
std::optional<Artwork> shared_cover(const std::vector<Artwork>& candidates, HANDLE stop = nullptr,
                                  CatalogHttpGetter getter = {});

// Public-page fallback only. Invoke after the ordinary catalog misses so a
// cached page miss cannot hide a recovered normal-catalog result.
class CatalogResolver {
public:
    explicit CatalogResolver(std::string country = "US", CatalogHttpGetter getter = {},
                             std::chrono::milliseconds request_interval = std::chrono::milliseconds(3200));
    ~CatalogResolver();
    CatalogResolver(const CatalogResolver&) = delete;
    CatalogResolver& operator=(const CatalogResolver&) = delete;
    std::optional<Artwork> page_cover(const Track& track, HANDLE stop = nullptr);
    // Whether the most recent new/cached miss came from a temporary failure.
    bool retryable() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

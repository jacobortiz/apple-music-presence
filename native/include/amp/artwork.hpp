#pragma once

#include "amp/model.hpp"
#include "amp/http.hpp"
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace amp {
struct Settings;
struct ArtworkDependencies {
    // Optional seams for deterministic offline worker tests; production uses
    // the bounded public HTTP transport and native motion/hosting pipeline.
    std::function<HttpResponse(std::string_view, size_t, HANDLE)> http;
    std::function<std::optional<Artwork>(const Track&, std::string_view, HANDLE)> motion;
    std::chrono::milliseconds request_interval{3200};
};

// Called by the dedicated artwork worker, never by media polling or the UI.
class ArtworkResolver {
public:
    ArtworkResolver(const Settings& settings, std::filesystem::path data_dir,
                    std::function<void()> changed = {}, ArtworkDependencies dependencies = {});
    ~ArtworkResolver();
    ArtworkResolver(const ArtworkResolver&) = delete;
    ArtworkResolver& operator=(const ArtworkResolver&) = delete;
    std::optional<Artwork> resolve(const Track& track);
    // Non-blocking: only a previously verified song can reuse/queue motion.
    std::optional<Artwork> refresh(const Track& track);
    std::string status(const Track& track);
    void cancel() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pure validation functions also used by the offline regression tests.
namespace artwork_detail {
std::string normalize(std::string_view value);
std::optional<std::string> thumbnail(std::string_view value);
std::optional<std::string> album_key(std::string_view page);
std::optional<Artwork> mapped_cover(std::string_view payload, const Track& track);
std::optional<Artwork> cached_motion(std::string_view payload, std::string_view repository,
                                    std::string_view verified_page, double now);
}
}

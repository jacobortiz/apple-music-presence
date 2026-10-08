#pragma once
#include <optional>
#include <string>

namespace amp {
enum class PlaybackState { stopped, paused, playing };
struct Track {
    std::string title, artist, album;
    bool operator==(const Track&) const = default;
};
struct Snapshot {
    std::optional<Track> track;
    PlaybackState state{PlaybackState::stopped};
    std::optional<double> position, duration;
    double observed_at{}, playback_rate{1.0};
    std::string source_id;
};
struct Artwork {
    std::string url, track_url;
    bool animated{};
    bool operator==(const Artwork&) const = default;
};
}

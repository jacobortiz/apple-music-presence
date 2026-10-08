#pragma once

#include <Windows.h>

#include <memory>
#include <string>
#include <string_view>

#include "amp/model.hpp"

namespace amp {

// Construct, read, and destroy on the same MTA worker thread. The caller owns
// its COM apartment and event; callbacks retain their own duplicate of the event.
class MediaBackend {
public:
    explicit MediaBackend(HANDLE wake_event, const std::string& source_id = {});
    ~MediaBackend();
    MediaBackend(const MediaBackend&) = delete;
    MediaBackend& operator=(const MediaBackend&) = delete;

    Snapshot read();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace media_detail {
bool is_apple_music_source(std::string_view source_id);
void normalize_artist_album(Track& track, std::string_view source_id);
void apply_timeline(Snapshot& snapshot, double start, double end,
                    double position, double updated_at);
}

}

#include "amp/media.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amp {
namespace {
using namespace winrt::Windows::Media::Control;
using namespace std::chrono_literals;

std::string lower_ascii(std::string_view value) {
    std::string result(value);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return result;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n\f\v");
    return std::string(value.substr(first, last - first + 1));
}

PlaybackState playback_state(GlobalSystemMediaTransportControlsSessionPlaybackStatus status) {
    switch (status) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
        return PlaybackState::playing;
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
        return PlaybackState::paused;
    default:
        return PlaybackState::stopped;
    }
}

int priority(PlaybackState state) {
    return state == PlaybackState::playing ? 0 : state == PlaybackState::paused ? 1 : 2;
}

double unix_seconds(winrt::Windows::Foundation::DateTime time) {
    return std::chrono::duration<double>(winrt::clock::to_sys(time).time_since_epoch()).count();
}

template<class Operation>
auto bounded_result(const Operation& operation) {
    if (operation.wait_for(5s) == winrt::Windows::Foundation::AsyncStatus::Started) {
        operation.Cancel();
        throw std::runtime_error("Windows media request timed out; retrying later.");
    }
    return operation.GetResults();
}

struct WakeSignal {
    winrt::handle event;
    std::atomic<bool> metadata_changed{true};

    explicit WakeSignal(HANDLE original) {
        HANDLE duplicate{};
        winrt::check_bool(DuplicateHandle(GetCurrentProcess(), original,
                                         GetCurrentProcess(), &duplicate,
                                         0, FALSE, DUPLICATE_SAME_ACCESS));
        event.attach(duplicate);
    }

    void notify(bool metadata = false) noexcept {
        if (metadata) metadata_changed.store(true, std::memory_order_relaxed);
        SetEvent(event.get());
    }
};

struct SessionWatch {
    GlobalSystemMediaTransportControlsSession session{nullptr};
    GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker metadata;
    GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker playback;
    GlobalSystemMediaTransportControlsSession::TimelinePropertiesChanged_revoker timeline;
};
}

namespace media_detail {
bool is_apple_music_source(std::string_view source_id) {
    const auto normalized = lower_ascii(source_id);
    if (normalized.starts_with("appleinc.applemusicwin_")) return true;
    const auto separator = normalized.find_last_of("\\/");
    return normalized.substr(separator == std::string::npos ? 0 : separator + 1) == "applemusic.exe";
}

void normalize_artist_album(Track& track, std::string_view source_id) {
    if (!track.album.empty() || !is_apple_music_source(source_id)) return;
    constexpr std::string_view separator = " \xE2\x80\x94 ";
    const auto split = track.artist.find(separator);
    if (split == std::string::npos ||
        track.artist.find(separator, split + separator.size()) != std::string::npos) return;
    auto artist = trim(std::string_view(track.artist).substr(0, split));
    auto album = trim(std::string_view(track.artist).substr(split + separator.size()));
    if (!artist.empty() && !album.empty()) {
        track.artist = std::move(artist);
        track.album = std::move(album);
    }
}

void apply_timeline(Snapshot& snapshot, double start, double end,
                    double position, double updated_at) {
    snapshot.position.reset();
    snapshot.duration.reset();
    if (!std::isfinite(start)) return;
    if (std::isfinite(end) && end > start && std::isfinite(end - start)) {
        snapshot.duration = end - start;
    }
    if (!std::isfinite(position) || !std::isfinite(position - start)) return;
    position = std::max(0.0, position - start);
    const auto rate = std::isfinite(snapshot.playback_rate) && snapshot.playback_rate > 0
        ? snapshot.playback_rate : 1.0;
    if (snapshot.state == PlaybackState::playing && std::isfinite(updated_at) &&
        updated_at > 0 && std::isfinite(snapshot.observed_at)) {
        position += std::max(0.0, snapshot.observed_at - updated_at) * rate;
    }
    if (snapshot.duration) position = std::min(position, *snapshot.duration);
    if (std::isfinite(position)) snapshot.position = position;
}
}

struct MediaBackend::Impl {
    const DWORD owner_thread{GetCurrentThreadId()};
    const std::string requested_source;
    std::shared_ptr<WakeSignal> signal;
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessions_changed;
    GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker current_changed;
    std::vector<SessionWatch> watches;
    GlobalSystemMediaTransportControlsSession cached_session{nullptr};
    std::optional<Track> cached_track;
    std::chrono::steady_clock::time_point metadata_time{};

    Impl(HANDLE wake_event, const std::string& source_id)
        : requested_source(lower_ascii(source_id)), signal(std::make_shared<WakeSignal>(wake_event)) {}

    bool matches(std::string_view source) const {
        return requested_source.empty() ? media_detail::is_apple_music_source(source)
                                        : lower_ascii(source) == requested_source;
    }

    void reset() noexcept {
        // Event handlers never capture Impl. A queued callback can safely finish
        // after revocation because it owns a duplicate of the kernel event.
        watches.clear();
        sessions_changed.revoke();
        current_changed.revoke();
        manager = nullptr;
        cached_session = nullptr;
        cached_track.reset();
        signal->metadata_changed.store(true, std::memory_order_relaxed);
    }

    void ensure_manager() {
        if (manager) return;
        manager = bounded_result(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
        sessions_changed = manager.SessionsChanged(winrt::auto_revoke,
            [wake = signal](auto const&, auto const&) { wake->notify(true); });
        current_changed = manager.CurrentSessionChanged(winrt::auto_revoke,
            [wake = signal](auto const&, auto const&) { wake->notify(true); });
    }

    Snapshot read_once() {
        ensure_manager();
        std::vector<GlobalSystemMediaTransportControlsSession> sessions;
        GlobalSystemMediaTransportControlsSession chosen{nullptr};
        PlaybackState chosen_state{PlaybackState::stopped};
        std::string chosen_source;
        for (const auto& session : manager.GetSessions()) {
            const auto source = winrt::to_string(session.SourceAppUserModelId());
            if (!matches(source)) continue;
            sessions.push_back(session);
            const auto state = playback_state(session.GetPlaybackInfo().PlaybackStatus());
            if (!chosen || priority(state) < priority(chosen_state) ||
                (state == chosen_state && source < chosen_source)) {
                chosen = session;
                chosen_state = state;
                chosen_source = source;
            }
        }

        std::erase_if(watches, [&sessions](const SessionWatch& watch) {
            return std::find(sessions.begin(), sessions.end(), watch.session) == sessions.end();
        });
        for (const auto& session : sessions) {
            if (std::any_of(watches.begin(), watches.end(), [&session](const SessionWatch& watch) {
                return watch.session == session;
            })) continue;
            SessionWatch watch;
            watch.session = session;
            watch.metadata = session.MediaPropertiesChanged(winrt::auto_revoke,
                [wake = signal](auto const&, auto const&) { wake->notify(true); });
            watch.playback = session.PlaybackInfoChanged(winrt::auto_revoke,
                [wake = signal](auto const&, auto const&) { wake->notify(); });
            watch.timeline = session.TimelinePropertiesChanged(winrt::auto_revoke,
                [wake = signal](auto const&, auto const&) { wake->notify(); });
            watches.push_back(std::move(watch));
        }

        Snapshot snapshot;
        snapshot.source_id = chosen_source;
        if (!chosen || chosen_state == PlaybackState::stopped) {
            cached_session = nullptr;
            cached_track.reset();
            return snapshot;
        }

        const auto metadata_changed = signal->metadata_changed.exchange(false, std::memory_order_relaxed);
        if (chosen != cached_session || !cached_track || metadata_changed ||
            std::chrono::steady_clock::now() - metadata_time >= 30s) {
            const auto properties = bounded_result(chosen.TryGetMediaPropertiesAsync());
            cached_session = chosen;
            cached_track.reset();
            metadata_time = std::chrono::steady_clock::now();
            if (properties) {
                Track track{trim(winrt::to_string(properties.Title())),
                            trim(winrt::to_string(properties.Artist())),
                            trim(winrt::to_string(properties.AlbumTitle()))};
                media_detail::normalize_artist_album(track, chosen_source);
                if (!track.title.empty()) cached_track = std::move(track);
            }
        }

        // State may have changed while Windows was fetching metadata.
        const auto playback = chosen.GetPlaybackInfo();
        snapshot.state = playback_state(playback.PlaybackStatus());
        if (!cached_track || snapshot.state == PlaybackState::stopped) {
            snapshot.state = PlaybackState::stopped;
            return snapshot;
        }
        snapshot.track = cached_track;
        snapshot.observed_at = unix_seconds(winrt::clock::now());
        if (const auto rate = playback.PlaybackRate()) {
            const auto value = rate.Value();
            if (std::isfinite(value) && value > 0) snapshot.playback_rate = value;
        }
        try {
            const auto timeline = chosen.GetTimelineProperties();
            media_detail::apply_timeline(snapshot,
                std::chrono::duration<double>(timeline.StartTime()).count(),
                std::chrono::duration<double>(timeline.EndTime()).count(),
                std::chrono::duration<double>(timeline.Position()).count(),
                unix_seconds(timeline.LastUpdatedTime()));
        } catch (const winrt::hresult_error&) {
            // Streams without timelines still have useful track metadata.
        }
        return snapshot;
    }
};

MediaBackend::MediaBackend(HANDLE wake_event, const std::string& source_id)
    : impl_(std::make_unique<Impl>(wake_event, source_id)) {}

MediaBackend::~MediaBackend() {
    impl_->reset();
}

Snapshot MediaBackend::read() {
    if (GetCurrentThreadId() != impl_->owner_thread) {
        throw std::logic_error("Use the media backend on its owning worker thread.");
    }
    try {
        return impl_->read_once();
    } catch (const winrt::hresult_error&) {
        impl_->reset();
        throw std::runtime_error("Windows media sessions are unavailable; retrying later.");
    } catch (...) {
        impl_->reset();
        throw;
    }
}

}

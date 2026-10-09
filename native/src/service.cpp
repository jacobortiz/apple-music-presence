#include <amp/service.hpp>
#include <amp/activity_schedule.hpp>
#include <amp/artwork.hpp>
#include <amp/discord_rpc.hpp>
#include <amp/media.hpp>
#include <amp/presence.hpp>
#include <winrt/base.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>

namespace amp {
namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point value) { return std::chrono::duration<double>(value.time_since_epoch()).count(); }
struct ArtResult { std::optional<Track> track; std::optional<Artwork> cover; };
class ArtWorker {
public:
    ArtWorker(const Settings& settings, const std::filesystem::path& directory, HANDLE wake)
        : resolver_(settings, directory, [wake] { SetEvent(wake); }), wake_(wake), thread_([this] { run(); }) {}
    ~ArtWorker() {
        { std::lock_guard lock(mutex_); stopped_ = true; pending_.reset(); }
        resolver_.cancel();
        changed_.notify_one(); thread_.join();
    }
    void request(const Track& track) {
        { std::lock_guard lock(mutex_); pending_ = track; result_ = {}; }
        changed_.notify_one();
    }
    ArtResult result() { std::lock_guard lock(mutex_); return result_; }
    void cancel() noexcept { resolver_.cancel(); }
    std::optional<Artwork> refresh(const Track& track) { return resolver_.refresh(track); }
    std::string status(const Track& track) { return resolver_.status(track); }
private:
    void run() {
        for (;;) {
            Track track;
            { std::unique_lock lock(mutex_); changed_.wait(lock, [&] { return stopped_ || pending_.has_value(); });
              if (stopped_) return; track = *pending_; pending_.reset(); }
            std::optional<Artwork> cover;
            try { cover = resolver_.resolve(track); } catch (...) { /* Optional artwork cannot stop presence. */ }
            { std::lock_guard lock(mutex_); if (stopped_) return; result_ = {track, cover}; }
            SetEvent(wake_);
        }
    }
    ArtworkResolver resolver_;
    HANDLE wake_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopped_{};
    std::optional<Track> pending_;
    ArtResult result_;
    std::thread thread_;
};
}
Service::Service(Settings settings, std::filesystem::path directory, std::function<void(const Status&)> notify, bool demo)
    : settings_(std::move(settings)), directory_(std::move(directory)), notify_(std::move(notify)), demo_(demo) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event_ || !wake_event_) {
        if (stop_event_) CloseHandle(stop_event_); if (wake_event_) CloseHandle(wake_event_);
        throw std::runtime_error("Could not start background worker");
    }
    try { worker_ = std::thread([this] { run(); }); }
    catch (...) { CloseHandle(stop_event_); CloseHandle(wake_event_); throw; }
}
Service::~Service() { stop(); CloseHandle(stop_event_); CloseHandle(wake_event_); }
void Service::configure(const Settings& settings) {
    { std::lock_guard lock(mutex_); settings_ = settings; ++version_; }
    SetEvent(wake_event_);
}
void Service::pause(bool value) { paused_ = value; SetEvent(wake_event_); }
void Service::stop() { SetEvent(stop_event_); SetEvent(wake_event_); if (worker_.joinable()) worker_.join(); }
void Service::run() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        HANDLE events[]{stop_event_, wake_event_};
        ActivitySchedule updates; // Reconnects and settings changes retain the same rate budget.
        while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
            Settings settings; unsigned version;
            { std::lock_guard lock(mutex_); settings = settings_; version = version_; }
            try { if (!demo_) validate_settings(settings); }
            catch (const std::exception& error) {
                notify_(Status{{}, error.what()});
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) break;
                continue;
            }
            updates.disconnected();
            DiscordRpc rpc(demo_ ? "0" : settings.client_id, stop_event_);
            std::unique_ptr<MediaBackend> media;
            std::unique_ptr<ArtWorker> artwork;
            if (!demo_ && settings.artwork) artwork = std::make_unique<ArtWorker>(settings, directory_, wake_event_);
            std::optional<Track> art_track;
            std::optional<Artwork> cover;
            bool art_pending{}, had_result{};
            double next_connect = 0, retry = 1, art_retry = 0, art_delay = 10, grace = 0;
            const double demo_start = unix_time();
            for (;;) {
                if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
                { std::lock_guard lock(mutex_); if (version != version_) break; }
                double now = seconds(Clock::now());
                Snapshot snapshot;
                std::string message = "Waiting for Apple Music";
                try {
                    if (demo_) {
                        double elapsed = unix_time() - demo_start;
                        snapshot = {Track{"A quiet afternoon", "Sample Artist", "Demo Album"},
                            elapsed >= 35 && elapsed < 43 ? PlaybackState::paused : PlaybackState::playing,
                            elapsed, 180.0, unix_time(), 1.0, "Offline demo"};
                    } else {
                        if (!media) media = std::make_unique<MediaBackend>(wake_event_, settings.source_id);
                        snapshot = media->read();
                    }
                } catch (...) { media.reset(); message = "Apple Music unavailable; retrying automatically"; }
                bool playing = snapshot.state == PlaybackState::playing && snapshot.track && !paused_;
                if (snapshot.track != art_track) {
                    art_track = snapshot.track; cover.reset(); art_pending = false; had_result = false;
                    art_retry = 0; art_delay = 10; grace = artwork && playing ? now + .75 : 0;
                }
                if (artwork && playing) {
                    if (!art_pending && !cover && now >= art_retry) {
                        artwork->request(*snapshot.track); art_pending = true;
                    }
                    auto result = artwork->result();
                    if (art_pending && result.track == snapshot.track) {
                        cover = result.cover; art_pending = false; had_result = true;
                        art_retry = now + art_delay; art_delay = std::min(60.0, art_delay * 2);
                    }
                    if (auto animated = artwork->refresh(*snapshot.track)) cover = std::move(animated);
                }
                bool connected = demo_ || rpc.connected();
                if (!connected && now >= next_connect) {
                    try { rpc.connect(); connected = true; updates.disconnected(); retry = 1; }
                    catch (...) { next_connect = seconds(Clock::now()) + retry; retry = std::min(30.0, retry * 2); }
                }
                now = seconds(Clock::now()); // Media reads and IPC connection setup can take time.
                auto effective = snapshot;
                if (paused_) effective.state = PlaybackState::paused;
                auto activity = presence(effective, unix_time(), cover);
                if (connected) {
                    try {
                        if (updates.due(activity, now) && (activity.is_null() || !art_pending || cover || now >= grace)) {
                            // Count attempts before IPC: a lost acknowledgement may still have applied the update.
                            updates.attempt(activity, now);
                            if (!demo_) { if (activity.is_null()) rpc.clear(); else rpc.update(activity); }
                            updates.acknowledge(activity, seconds(Clock::now()));
                        }
                    } catch (...) {
                        rpc.close(); connected = false; updates.disconnected();
                        next_connect = seconds(Clock::now()) + retry; retry = std::min(30.0, retry * 2);
                    }
                }
                if (paused_) message = "Sharing paused";
                else if (playing) message = connected ? (demo_ ? "Offline preview" : "Sharing with Discord") : "Discord unavailable; retrying automatically";
                else if (snapshot.state == PlaybackState::paused) message = updates.may_be_active()
                    ? "Playback paused; clearing activity" : "Playback paused; activity cleared";
                std::string art_status = !artwork ? "Artwork off" : art_pending ? "Looking up artwork" : cover ?
                    (cover->animated ? "Using a hosted animated cover" : "Normal artwork ready") :
                    had_result ? "No verified cover; retrying during playback" : "Artwork waits for playback";
                if (artwork && playing) {
                    auto motion_status = artwork->status(*snapshot.track);
                    if (!motion_status.empty()) art_status = std::move(motion_status);
                }
                notify_(Status{snapshot, message, art_status, connected, paused_});
                // Native events wake the worker immediately. A 30s heartbeat
                // also detects missed media events and idle Discord restarts.
                now = seconds(Clock::now());
                double deadline = now + (demo_ ? 1 : 30);
                if (!connected) deadline = std::min(deadline, next_connect);
                if (connected) {
                    auto next_update = updates.next(activity, now);
                    if (!activity.is_null() && art_pending && !cover) next_update = std::max(next_update, grace);
                    deadline = std::min(deadline, next_update);
                }
                if (artwork && playing && !cover && !art_pending) deadline = std::min(deadline, art_retry);
                auto delay = static_cast<DWORD>(std::clamp((deadline - now) * 1000.0, 20.0, 30000.0));
                if (WaitForMultipleObjects(2, events, FALSE, delay) == WAIT_OBJECT_0) break;
            }
            if (artwork) artwork->cancel();
            try {
                const auto now = seconds(Clock::now());
                // Exit promptly if the budget is exhausted. Closing IPC never creates another update.
                if (!demo_ && rpc.connected() && updates.may_be_active() && updates.can_clear(now)) {
                    updates.attempt(nullptr, now);
                    rpc.clear(); updates.acknowledge(nullptr, seconds(Clock::now()));
                }
            } catch (...) {}
            rpc.close();
        }
        winrt::uninit_apartment();
    } catch (const std::exception& error) { notify_(Status{{}, std::string("Unable to start: ") + error.what()}); }
    catch (...) { notify_(Status{{}, "Windows media service unavailable"}); }
}
}

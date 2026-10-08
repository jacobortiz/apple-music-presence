#include "amp/service.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
            (L"amp-service-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(path), "Could not create service test directory.");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    std::filesystem::path path;
};

class StatusLog {
public:
    void receive(const amp::Status& status) {
        { std::lock_guard lock(mutex_); values_.push_back(status); }
        changed_.notify_all();
    }

    std::size_t size() {
        std::lock_guard lock(mutex_);
        return values_.size();
    }

    template<class Predicate>
    amp::Status wait_after(std::size_t index, Predicate predicate) {
        std::unique_lock lock(mutex_);
        amp::Status match;
        const bool found = changed_.wait_for(lock, 2s, [&] {
            for (auto current = index; current < values_.size(); ++current) {
                require(!values_[current].message.starts_with("Unable to start:"),
                        "Offline service failed to start.");
                if (predicate(values_[current])) { match = values_[current]; return true; }
            }
            return false;
        });
        require(found, "Offline service did not produce the expected status.");
        return match;
    }

    void require_quiet_for(std::size_t index, std::chrono::milliseconds duration) {
        std::unique_lock lock(mutex_);
        const bool changed = changed_.wait_for(lock, duration, [&] { return values_.size() != index; });
        require(!changed, "Service emitted a callback after stop returned.");
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<amp::Status> values_;
};
} // namespace

void test_service() {
    TemporaryDirectory directory;
    StatusLog log;
    amp::Settings settings; // Offline preview needs no configured Discord application.
    {
        amp::Service service(settings, directory.path,
            [&](const amp::Status& status) { log.receive(status); }, true);
        const auto first = log.wait_after(0, [](const auto& status) {
            return status.message == "Offline preview";
        });
        require(first.snapshot.track && first.snapshot.track->title == "A quiet afternoon" &&
                first.snapshot.track->artist == "Sample Artist" &&
                first.snapshot.track->album == "Demo Album", "Offline preview metadata is missing.");
        require(first.snapshot.source_id == "Offline demo" && first.snapshot.duration == 180.0,
                "Offline preview did not use its own media snapshot.");
        require(first.artwork_status == "Artwork off" && !first.paused,
                "Offline preview incorrectly started artwork or sharing pause.");

        auto mark = log.size();
        service.pause(true);
        log.wait_after(mark, [](const auto& status) {
            return status.paused && status.message == "Sharing paused";
        });

        // Live preferences must never enable media, artwork or Discord in demo.
        settings.client_id = "invalid ID that must not be used";
        settings.country = "??";
        settings.artwork = true;
        settings.motion_artwork = true;
        settings.artwork_repository = "invalid repository";
        mark = log.size();
        service.configure(settings);
        const auto reconfigured = log.wait_after(mark, [](const auto& status) {
            return status.paused && status.message == "Sharing paused";
        });
        require(reconfigured.artwork_status == "Artwork off",
                "Reconfiguring offline preview started live artwork.");

        mark = log.size();
        service.pause(false);
        const auto resumed = log.wait_after(mark, [](const auto& status) {
            return !status.paused && status.message == "Offline preview";
        });
        mark = log.size();
        const auto progressed = log.wait_after(mark, [&](const auto& status) {
            return status.snapshot.observed_at >= resumed.snapshot.observed_at + .5;
        });
        require(progressed.snapshot.position && resumed.snapshot.position &&
                *progressed.snapshot.position > *resumed.snapshot.position,
                "Offline preview progress did not advance.");
        require(log.size() - mark <= 6, "Offline service is spinning instead of sleeping between updates.");

        const auto stopping = std::chrono::steady_clock::now();
        service.stop();
        require(std::chrono::steady_clock::now() - stopping < 500ms,
                "Offline service stop did not promptly wake the worker.");
        service.stop(); // Repeated stop and the following destructor are safe.
        log.require_quiet_for(log.size(), 100ms);
    }
    require(std::filesystem::is_empty(directory.path),
            "Offline preview wrote preferences or an artwork cache.");
}

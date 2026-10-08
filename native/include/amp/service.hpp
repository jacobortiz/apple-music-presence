#pragma once
#include <amp/model.hpp>
#include <amp/settings.hpp>
#include <Windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace amp {
struct Status {
    Snapshot snapshot;
    std::string message{"Starting"}, artwork_status;
    bool connected{}, paused{};
};
class Service {
public:
    Service(Settings, std::filesystem::path, std::function<void(const Status&)>, bool demo = false);
    ~Service();
    void configure(const Settings&);
    void pause(bool);
    void stop();
private:
    void run();
    Settings settings_;
    std::filesystem::path directory_;
    std::function<void(const Status&)> notify_;
    bool demo_{};
    std::mutex mutex_;
    unsigned version_{};
    std::atomic_bool paused_{};
    HANDLE stop_event_{}, wake_event_{};
    std::thread worker_;
};
}

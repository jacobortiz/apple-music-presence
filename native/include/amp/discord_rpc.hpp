#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace amp {

// One local IPC connection, owned and called by the service worker thread.
// The stop event is borrowed; clear() deliberately ignores it for final cleanup.
class DiscordRpc {
public:
    explicit DiscordRpc(
        std::string client_id,
        HANDLE stop_event = nullptr,
        std::wstring pipe_prefix = L"\\\\?\\pipe\\discord-ipc-",
        std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~DiscordRpc();
    DiscordRpc(const DiscordRpc&) = delete;
    DiscordRpc& operator=(const DiscordRpc&) = delete;

    void connect();
    void update(const nlohmann::json& activity);
    void clear();
    void close() noexcept;
    bool connected() const noexcept;

private:
    using Deadline = std::chrono::steady_clock::time_point;
    std::string client_id_;
    HANDLE stop_event_;
    std::wstring pipe_prefix_;
    std::chrono::milliseconds timeout_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool connected_ = false;
    std::uint64_t nonce_ = 0;

    void check_wait(Deadline deadline, bool cancellable) const;
    void transfer(void* buffer, std::size_t length, bool writing,
                  Deadline deadline, bool cancellable);
    void write_frame(std::uint32_t opcode, const std::string& body,
                     Deadline deadline, bool cancellable);
    std::pair<std::uint32_t, std::string> read_frame(
        Deadline deadline, bool cancellable);
    nlohmann::json response(const std::string& nonce,
                            Deadline deadline, bool cancellable);
    void set_activity(const nlohmann::json& activity, bool cancellable);
};

} // namespace amp

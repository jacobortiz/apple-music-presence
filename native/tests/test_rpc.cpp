#include "amp/discord_rpc.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void must_fail(Operation operation) {
    bool failed = false;
    try { operation(); } catch (const std::exception&) { failed = true; }
    require(failed, "Discord RPC unexpectedly accepted a failed operation.");
}

std::uint32_t u32(const unsigned char* value) {
    return static_cast<std::uint32_t>(value[0]) |
           (static_cast<std::uint32_t>(value[1]) << 8) |
           (static_cast<std::uint32_t>(value[2]) << 16) |
           (static_cast<std::uint32_t>(value[3]) << 24);
}

void put_u32(unsigned char* bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        *bytes++ = static_cast<unsigned char>(value >> shift);
}

std::vector<unsigned char> packet(std::uint32_t opcode, const std::string& body) {
    std::vector<unsigned char> result(body.size() + 8);
    put_u32(result.data(), opcode);
    put_u32(result.data() + 4, static_cast<std::uint32_t>(body.size()));
    std::copy(body.begin(), body.end(), result.begin() + 8);
    return result;
}

// These tests create their own uniquely named pipe and never touch Discord.
class FakeServer {
public:
    using Script = std::function<void(FakeServer&)>;
    explicit FakeServer(Script script, int index = 0, std::wstring name_prefix = {}) {
        static std::atomic<unsigned> next{0};
        prefix = name_prefix.empty()
            ? L"\\\\?\\pipe\\amp-rpc-test-" + std::to_wstring(GetCurrentProcessId()) +
              L"-" + std::to_wstring(++next) + L"-"
            : std::move(name_prefix);
        const auto name = prefix + std::to_wstring(index);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pipe_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                 1, 8192, 8192, 0, nullptr);
        if (!stop_ || pipe_ == INVALID_HANDLE_VALUE) {
            if (stop_) CloseHandle(stop_);
            if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
            throw std::runtime_error("Could not create test IPC pipe.");
        }
        thread_ = std::thread([this, script = std::move(script)] {
            try {
                OVERLAPPED operation{};
                operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!operation.hEvent) throw std::runtime_error("Test IPC event failed.");
                const bool connected = ConnectNamedPipe(pipe_, &operation) != FALSE;
                const auto error = connected ? ERROR_SUCCESS : GetLastError();
                try {
                    if (error == ERROR_IO_PENDING) finish(operation);
                    else if (error != ERROR_PIPE_CONNECTED && error != ERROR_SUCCESS)
                        throw std::runtime_error("Test IPC connection failed.");
                } catch (...) {
                    CloseHandle(operation.hEvent);
                    throw;
                }
                CloseHandle(operation.hEvent);
                script(*this);
            } catch (...) { error_ = std::current_exception(); }
        });
    }
    ~FakeServer() {
        SetEvent(stop_);
        CancelIoEx(pipe_, nullptr);
        if (thread_.joinable()) thread_.join();
        CloseHandle(pipe_);
        CloseHandle(stop_);
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    void wait(bool check = true) {
        if (thread_.joinable()) thread_.join();
        if (check && error_) std::rethrow_exception(error_);
    }

    void transfer(void* data, std::size_t length, bool writing) {
        auto* bytes = static_cast<unsigned char*>(data);
        while (length) {
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            require(operation.hEvent != nullptr, "Test IPC event failed.");
            DWORD count = 0;
            try {
                const bool success = writing
                    ? WriteFile(pipe_, bytes, static_cast<DWORD>(length), &count, &operation) != FALSE
                    : ReadFile(pipe_, bytes, static_cast<DWORD>(length), &count, &operation) != FALSE;
                if (!success) {
                    require(GetLastError() == ERROR_IO_PENDING, "Test IPC transfer failed.");
                    count = finish(operation);
                }
                require(count > 0 && count <= length, "Test IPC ended unexpectedly.");
            } catch (...) {
                CloseHandle(operation.hEvent);
                throw;
            }
            CloseHandle(operation.hEvent);
            bytes += count;
            length -= count;
        }
    }

    void send(std::uint32_t opcode, const std::string& body, bool fragmented = false) {
        auto bytes = packet(opcode, body);
        if (fragmented) {
            transfer(bytes.data(), 3, true);
            std::this_thread::sleep_for(2ms);
            transfer(bytes.data() + 3, 5, true);
            std::this_thread::sleep_for(2ms);
            transfer(bytes.data() + 8, bytes.size() - 8, true);
        } else transfer(bytes.data(), bytes.size(), true);
    }

    std::pair<std::uint32_t, std::string> receive() {
        std::array<unsigned char, 8> header{};
        transfer(header.data(), header.size(), false);
        const auto length = u32(header.data() + 4);
        require(length <= 1'048'576, "Test IPC received an oversized frame.");
        std::string body(length, '\0');
        transfer(body.data(), body.size(), false);
        return {u32(header.data()), body};
    }

    void ready(bool fragmented = false) {
        const auto [opcode, body] = receive();
        const auto handshake = Json::parse(body);
        require(opcode == 0 && handshake.at("v") == 1 &&
                handshake.at("client_id") == "123456789012345678", "Invalid IPC handshake.");
        send(1, Json{{"evt", "ERROR"}, {"nonce", "an-unrelated-request"}}.dump());
        send(1, Json{{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", {{"v", 1}}}}.dump(), fragmented);
    }

    Json activity() {
        const auto [opcode, body] = receive();
        const auto request = Json::parse(body);
        require(opcode == 1 && request.at("cmd") == "SET_ACTIVITY", "Invalid activity command.");
        require(request.at("args").at("pid") == GetCurrentProcessId(), "Activity has the wrong PID.");
        return request;
    }

    void acknowledge(const Json& request) {
        send(1, Json{{"cmd", "SET_ACTIVITY"}, {"nonce", request.at("nonce")}, {"data", nullptr}}.dump(), true);
    }

    std::wstring prefix;

private:
    DWORD finish(OVERLAPPED& operation) {
        HANDLE waits[] = {operation.hEvent, stop_};
        const auto result = WaitForMultipleObjects(2, waits, FALSE, 3000);
        DWORD count = 0;
        if (result != WAIT_OBJECT_0) {
            CancelIoEx(pipe_, &operation);
            GetOverlappedResult(pipe_, &operation, &count, TRUE);
            throw std::runtime_error("Test IPC timed out or was stopped.");
        }
        require(GetOverlappedResult(pipe_, &operation, &count, FALSE) != FALSE,
                "Test IPC operation failed.");
        return count;
    }

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stop_ = nullptr;
    std::thread thread_;
    std::exception_ptr error_;
};

void test_frames_and_clear() {
    const std::string ping("raw\0ping\xff", 9);
    FakeServer server([&](FakeServer& peer) {
        peer.ready(true);
        const auto update = peer.activity();
        require(update.at("args").at("activity").at("type") == 2, "Listening type was lost.");
        require(update.at("args").at("activity").at("state") == "Artist", "Artist was lost.");
        peer.send(1, Json{{"evt", "ERROR"}, {"nonce", "other"}}.dump());
        peer.send(1, Json{{"cmd", "DISPATCH"}, {"evt", "CURRENT_USER_UPDATE"}}.dump());
        peer.send(3, ping, true);
        const auto [opcode, body] = peer.receive();
        require(opcode == 4 && body == ping, "PING must be echoed byte-for-byte.");
        peer.send(4, "ignored pong");
        peer.acknowledge(update);
        const auto clear = peer.activity();
        require(clear.at("args").contains("activity") && clear.at("args").at("activity").is_null(),
                "Clear must preserve explicit activity:null.");
        require(clear.at("nonce") != update.at("nonce"), "IPC nonce was reused.");
        peer.acknowledge(clear);
    }, 7); // Discovery must search beyond discord-ipc-0.
    amp::DiscordRpc rpc("123456789012345678", nullptr, server.prefix);
    rpc.connect();
    require(rpc.connected(), "RPC did not connect.");
    rpc.update(Json{{"type", 2}, {"state", "Artist"}, {"status_display_type", 1}});
    rpc.clear();
    server.wait();
    rpc.close();
    require(!rpc.connected(), "RPC close did not disconnect.");
    rpc.clear(); // Clearing a closed connection is harmless.
}

void test_rejected_request_and_reconnect() {
    const auto prefix = L"\\\\?\\pipe\\amp-rpc-reconnect-" +
                        std::to_wstring(GetCurrentProcessId()) + L"-";
    amp::DiscordRpc rpc("123456789012345678", nullptr, prefix);
    {
        FakeServer rejected([](FakeServer& peer) {
            peer.ready();
            const auto request = peer.activity();
            peer.send(1, Json{{"cmd", "SET_ACTIVITY"}, {"evt", "ERROR"},
                             {"nonce", request.at("nonce")}, {"data", {{"message", "Rejected"}}}}.dump());
        }, 0, prefix);
        rpc.connect();
        must_fail([&] { rpc.update(Json{{"type", 2}}); });
        require(!rpc.connected(), "Rejected request did not disconnect.");
        rejected.wait();
    }
    {
        FakeServer accepted([](FakeServer& peer) {
            peer.ready();
            peer.acknowledge(peer.activity());
        }, 0, prefix);
        rpc.connect();
        rpc.update(Json{{"type", 2}});
        accepted.wait();
    }
}

void test_bad_frames() {
    for (int variant = 0; variant != 5; ++variant) {
        FakeServer server([variant](FakeServer& peer) {
            peer.ready();
            peer.activity();
            if (variant == 0) {
                std::array<unsigned char, 8> header{};
                put_u32(header.data(), 1);
                put_u32(header.data() + 4, 1'048'577);
                peer.transfer(header.data(), header.size(), true);
            } else if (variant == 1) peer.send(2, "closed");
            else if (variant == 2) peer.send(88, "unknown");
            else if (variant == 3) peer.send(1, "not JSON");
            else peer.send(1, std::string(100, '[') + "0" + std::string(100, ']'));
        });
        amp::DiscordRpc rpc("123456789012345678", nullptr, server.prefix);
        rpc.connect();
        must_fail([&] { rpc.update(Json{{"type", 2}}); });
        require(!rpc.connected(), "Invalid response did not disconnect.");
        server.wait();
    }
}

void test_whole_request_deadline() {
    FakeServer server([](FakeServer& peer) {
        peer.ready();
        peer.activity();
        // Unrelated replies must not reset the original operation deadline.
        for (int index = 0; index != 10; ++index) {
            std::this_thread::sleep_for(60ms);
            peer.send(1, Json{{"cmd", "SET_ACTIVITY"}, {"nonce", "unrelated"}}.dump());
        }
    });
    amp::DiscordRpc rpc("123456789012345678", nullptr, server.prefix, 250ms);
    rpc.connect();
    const auto start = std::chrono::steady_clock::now();
    must_fail([&] { rpc.update(Json{{"type", 2}}); });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(elapsed >= 180ms && elapsed < 550ms, "IPC request deadline was extended.");
    require(!rpc.connected(), "Timed out request did not disconnect.");
    server.wait(false); // Expected client cancellation ends the scripted replies.
}

void test_cancellation_and_final_clear() {
    const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(stop != nullptr, "Could not create test stop event.");
    try {
        FakeServer server([](FakeServer& peer) {
            peer.ready();
            const auto clear = peer.activity();
            require(clear.at("args").at("activity").is_null(), "Expected final clear after cancellation.");
            peer.acknowledge(clear);
        });
        amp::DiscordRpc rpc("123456789012345678", stop, server.prefix);
        rpc.connect();
        SetEvent(stop);
        rpc.clear(); // Shutdown may clear even when the worker stop event is set.
        server.wait();
        must_fail([&] { rpc.update(Json{{"type", 2}}); });
        require(!rpc.connected(), "Cancelled update did not disconnect.");
    } catch (...) { CloseHandle(stop); throw; }
    CloseHandle(stop);

    const auto cancelled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(cancelled != nullptr, "Could not create test stop event.");
    try {
        FakeServer server([](FakeServer& peer) {
            peer.receive(); // Leave the handshake waiting for READY.
            std::this_thread::sleep_for(300ms);
        });
        amp::DiscordRpc rpc("123456789012345678", cancelled, server.prefix);
        std::jthread cancel([cancelled] {
            std::this_thread::sleep_for(40ms);
            SetEvent(cancelled);
        });
        const auto start = std::chrono::steady_clock::now();
        must_fail([&] { rpc.connect(); });
        cancel.join();
        require(std::chrono::steady_clock::now() - start < 250ms,
                "Pending IPC did not promptly respond to cancellation.");
        require(!rpc.connected(), "Cancelled handshake did not disconnect.");
        server.wait();
    } catch (...) { CloseHandle(cancelled); throw; }
    CloseHandle(cancelled);
}
} // namespace

void test_rpc() {
    must_fail([] { amp::DiscordRpc rpc("not-a-client-id"); });
    test_frames_and_clear();
    test_rejected_request_and_reconnect();
    test_bad_frames();
    test_whole_request_deadline();
    test_cancellation_and_final_clear();
}

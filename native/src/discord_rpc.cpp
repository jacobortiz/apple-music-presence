#include "amp/discord_rpc.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace amp {
namespace {
constexpr std::size_t max_frame = 1'048'576;

class Event {
public:
    Event() : handle(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!handle) throw std::runtime_error("Could not create Discord IPC event.");
    }
    ~Event() { CloseHandle(handle); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    HANDLE handle;
};

std::uint32_t read_u32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_u32(unsigned char* bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        *bytes++ = static_cast<unsigned char>(value >> shift);
}

DWORD remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= decltype(remaining)::zero())
        throw std::runtime_error("Discord IPC timed out.");
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    return static_cast<DWORD>(std::min<std::int64_t>(ms, MAXDWORD - 1));
}

nlohmann::json parse_response(const std::string& body) {
    // A byte limit alone does not bound deeply nested JSON parser recursion.
    auto result = nlohmann::json::parse(body, [](int depth, auto, auto&) {
        if (depth > 64) throw std::runtime_error("Discord IPC JSON is too deeply nested.");
        return true;
    });
    if (!result.is_object())
        throw std::runtime_error("Discord returned an invalid IPC response.");
    return result;
}
} // namespace

DiscordRpc::DiscordRpc(std::string client_id, HANDLE stop_event,
                       std::wstring pipe_prefix, std::chrono::milliseconds timeout)
    : client_id_(std::move(client_id)), stop_event_(stop_event),
      pipe_prefix_(std::move(pipe_prefix)), timeout_(timeout) {
    if (client_id_.empty() || !std::all_of(client_id_.begin(), client_id_.end(),
            [](unsigned char value) { return value >= '0' && value <= '9'; }))
        throw std::invalid_argument("Discord application ID must contain only digits.");
    if (timeout_.count() <= 0 || timeout_ > std::chrono::minutes(1))
        throw std::invalid_argument("Discord IPC timeout must be between 1 ms and 1 minute.");
    if (!pipe_prefix_.starts_with(L"\\\\?\\pipe\\") || pipe_prefix_.size() > 240)
        throw std::invalid_argument("Discord IPC requires a local Windows named pipe.");
}

DiscordRpc::~DiscordRpc() { close(); }

bool DiscordRpc::connected() const noexcept { return connected_; }

void DiscordRpc::close() noexcept {
    connected_ = false;
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}

void DiscordRpc::check_wait(Deadline deadline, bool cancellable) const {
    if (cancellable && stop_event_ && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0)
        throw std::runtime_error("Discord IPC was cancelled.");
    remaining_ms(deadline);
}

void DiscordRpc::transfer(void* buffer, std::size_t length, bool writing,
                          Deadline deadline, bool cancellable) {
    auto* bytes = static_cast<unsigned char*>(buffer);
    while (length) {
        check_wait(deadline, cancellable);
        Event event;
        OVERLAPPED operation{};
        operation.hEvent = event.handle;
        DWORD count = 0;
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(length, MAXDWORD));
        const BOOL success = writing
            ? WriteFile(pipe_, bytes, amount, &count, &operation)
            : ReadFile(pipe_, bytes, amount, &count, &operation);
        if (!success) {
            const auto error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                // The original request deadline also covers every header/body,
                // unsolicited event and PING/PONG, so peers cannot extend it.
                HANDLE waits[] = {event.handle, stop_event_};
                DWORD waited = WAIT_FAILED;
                try {
                    waited = WaitForMultipleObjects(cancellable && stop_event_ ? 2 : 1,
                                                    waits, FALSE, remaining_ms(deadline));
                } catch (...) {
                    CancelIoEx(pipe_, &operation);
                    GetOverlappedResult(pipe_, &operation, &count, TRUE);
                    throw;
                }
                if (waited != WAIT_OBJECT_0) {
                    // OVERLAPPED and its buffer must live until cancellation completes.
                    CancelIoEx(pipe_, &operation);
                    GetOverlappedResult(pipe_, &operation, &count, TRUE);
                    throw std::runtime_error(waited == WAIT_OBJECT_0 + 1
                        ? "Discord IPC was cancelled." : "Discord IPC timed out.");
                }
                if (!GetOverlappedResult(pipe_, &operation, &count, FALSE) &&
                    GetLastError() != ERROR_MORE_DATA)
                    throw std::runtime_error("Discord IPC connection was lost.");
            } else if (error != ERROR_MORE_DATA) {
                throw std::runtime_error("Discord IPC connection was lost.");
            }
        }
        if (!count || count > length)
            throw std::runtime_error("Discord IPC connection was lost.");
        bytes += count;
        length -= count;
    }
}

void DiscordRpc::write_frame(std::uint32_t opcode, const std::string& body,
                            Deadline deadline, bool cancellable) {
    if (body.size() > max_frame)
        throw std::runtime_error("Discord IPC frame exceeds the size limit.");
    std::vector<unsigned char> packet(8 + body.size());
    write_u32(packet.data(), opcode);
    write_u32(packet.data() + 4, static_cast<std::uint32_t>(body.size()));
    std::copy(body.begin(), body.end(), packet.begin() + 8);
    // Header and JSON are submitted together, as required by Discord IPC.
    transfer(packet.data(), packet.size(), true, deadline, cancellable);
}

std::pair<std::uint32_t, std::string> DiscordRpc::read_frame(
    Deadline deadline, bool cancellable) {
    std::array<unsigned char, 8> header{};
    transfer(header.data(), header.size(), false, deadline, cancellable);
    const auto length = read_u32(header.data() + 4);
    if (length > max_frame)
        throw std::runtime_error("Discord returned an oversized IPC frame.");
    std::string body(length, '\0');
    transfer(body.data(), body.size(), false, deadline, cancellable);
    return {read_u32(header.data()), std::move(body)};
}

nlohmann::json DiscordRpc::response(const std::string& nonce,
                                   Deadline deadline, bool cancellable) {
    for (;;) {
        check_wait(deadline, cancellable);
        const auto [opcode, body] = read_frame(deadline, cancellable);
        if (opcode == 3) {
            write_frame(4, body, deadline, cancellable); // Echo PING byte-for-byte.
            continue;
        }
        if (opcode == 4) continue;
        if (opcode == 2) throw std::runtime_error("Discord closed the IPC connection.");
        if (opcode != 1) throw std::runtime_error("Discord returned an unknown IPC opcode.");
        const auto value = parse_response(body);
        if (!nonce.empty() && (!value.contains("nonce") || value["nonce"] != nonce))
            continue; // Includes errors belonging to another request.
        if (nonce.empty() && value.contains("nonce") && !value["nonce"].is_null())
            continue; // READY and handshake errors are unsolicited, without a nonce.
        if (value.contains("evt") && value["evt"] == "ERROR")
            throw std::runtime_error("Discord rejected the activity request.");
        if (nonce.empty()) {
            if (value.contains("evt") && value["evt"] == "READY") return value;
        } else {
            if (!value.contains("cmd") || value["cmd"] != "SET_ACTIVITY")
                throw std::runtime_error("Discord returned an invalid activity acknowledgement.");
            return value;
        }
    }
}

void DiscordRpc::connect() {
    if (connected_) return;
    try {
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        for (int index = 0; index != 10; ++index) {
            check_wait(deadline, true);
            const auto name = pipe_prefix_ + std::to_wstring(index);
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) break;
        }
        if (pipe_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Discord desktop is not running or its IPC pipe is unavailable.");
        write_frame(0, nlohmann::json{{"v", 1}, {"client_id", client_id_}}.dump(), deadline, true);
        response("", deadline, true);
        connected_ = true;
    } catch (...) {
        close();
        throw;
    }
}

void DiscordRpc::set_activity(const nlohmann::json& activity, bool cancellable) {
    try {
        if (!connected_) throw std::runtime_error("Discord is not connected.");
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        const auto nonce = std::to_string(GetCurrentProcessId()) + ":" +
                           std::to_string(GetTickCount64()) + ":" + std::to_string(++nonce_);
        const nlohmann::json request{
            {"cmd", "SET_ACTIVITY"},
            {"args", {{"pid", GetCurrentProcessId()}, {"activity", activity}}},
            {"nonce", nonce}};
        write_frame(1, request.dump(), deadline, cancellable);
        response(nonce, deadline, cancellable);
    } catch (...) {
        close();
        throw;
    }
}

void DiscordRpc::update(const nlohmann::json& activity) {
    if (!activity.is_object()) {
        close();
        throw std::invalid_argument("Discord activity must be a JSON object.");
    }
    set_activity(activity, true);
}

void DiscordRpc::clear() {
    if (connected_) set_activity(nullptr, false); // Preserve explicit activity:null.
}

} // namespace amp

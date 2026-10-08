#include <amp/http.hpp>
#include <amp/settings.hpp>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>

namespace amp {
namespace {
using Clock = std::chrono::steady_clock;
struct Url { std::string host, target; };
std::optional<Url> split(std::string_view url) {
    if (!url.starts_with("https://") || url.size() > 8192 || url.find_first_of("\\#") != url.npos
        || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return {};
    auto end = url.find_first_of("/?", 8);
    auto host = std::string(url.substr(8, end == url.npos ? url.size() - 8 : end - 8));
    if (host.ends_with(":443")) host.resize(host.size() - 4);
    if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    })) return {};
    for (auto& c : host) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (!(host == "itunes.apple.com" || host == "music.apple.com" || host == "mvod.itunes.apple.com"
        || host.ends_with(".mzstatic.com") || host.ends_with(".itunes.apple.com")
        || host == "api.github.com" || host == "raw.githubusercontent.com")) return {};
    std::string target(end == url.npos ? "/" : url.substr(end));
    if (target.starts_with('?')) target.insert(target.begin(), '/');
    return Url{std::move(host), std::move(target)};
}
struct Internet {
    HINTERNET handle{};
    explicit Internet(HINTERNET value) : handle(value) { if (!handle) throw std::runtime_error("Network request unavailable"); }
    ~Internet() { WinHttpCloseHandle(handle); }
    Internet(const Internet&) = delete;
};
// WinHTTP can finish callbacks after cancellation. Everything used by a pending
// send/read belongs to this binding until its final HANDLE_CLOSING callback.
struct State {
    std::mutex mutex; std::condition_variable ready;
    DWORD status{}, bytes{}; bool failed{};
    std::array<char, 8192> buffer{};
    std::string body;
};
using Binding = std::shared_ptr<State>;
void CALLBACK complete(HINTERNET, DWORD_PTR context, DWORD status, void*, DWORD bytes) {
    if (!context) return;
    auto* binding = reinterpret_cast<Binding*>(context);
    auto state = *binding;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete binding; return; }
    { std::lock_guard lock(state->mutex);
      if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) state->failed = true;
      state->status = status; state->bytes = bytes; }
    state->ready.notify_all();
}
std::string location(HINTERNET request) {
    DWORD bytes{};
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                              nullptr, &bytes, WINHTTP_NO_HEADER_INDEX)) {
        if (GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND) return {};
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 4096) throw std::runtime_error("Invalid redirect header");
    }
    std::wstring result(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                            result.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) throw std::runtime_error("Invalid redirect header");
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return utf8(result);
}
}
namespace http_detail {
bool permitted_url(std::string_view url, bool authenticated) {
    auto value = split(url); return value && (!authenticated || value->host == "api.github.com");
}
}
HttpResponse http_get(std::string_view url, size_t limit, HANDLE stop) { return http_request(url, limit, "GET", "", "", stop); }
HttpResponse http_request(std::string_view url, size_t limit, std::string_view method,
                          std::string_view body, std::string_view token, HANDLE stop) {
    auto value = split(url);
    if (!value || (!token.empty() && value->host != "api.github.com") || !limit || limit > 16 * 1024 * 1024
        || body.size() > 12 * 1024 * 1024 || token.size() > 4096
        || std::any_of(token.begin(), token.end(), [](unsigned char c) { return c <= 32 || c >= 127; })
        || !(method == "GET" || method == "PUT" || method == "POST") || (method == "GET" && !body.empty()))
        throw std::runtime_error("Unsafe network request rejected");
    auto cancelled = [&] { return stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; };
    if (cancelled()) throw std::runtime_error("Network request cancelled");
    Internet session(WinHttpOpen(L"AppleMusicPresence-Native/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 5000);
    Internet connection(WinHttpConnect(session.handle, wide(value->host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    Internet request(WinHttpOpenRequest(connection.handle, wide(method).c_str(), wide(value->target).c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
        throw std::runtime_error("Network request options unavailable");
    auto state = std::make_shared<State>(); state->body = body;
    if (WinHttpSetStatusCallback(request.handle, complete, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0)
        == WINHTTP_INVALID_STATUS_CALLBACK) throw std::runtime_error("Network request callback unavailable");
    auto binding = std::make_unique<Binding>(state);
    auto context = reinterpret_cast<DWORD_PTR>(binding.get());
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        throw std::runtime_error("Network request context unavailable");
    binding.release();
    const auto deadline = Clock::now() + std::chrono::seconds(value->host == "api.github.com" ? 10 : 5);
    auto reset = [&] { std::lock_guard lock(state->mutex); state->status = 0; state->bytes = 0; };
    auto await = [&](BOOL started, DWORD wanted) {
        if (!started && GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Network request failed");
        std::unique_lock lock(state->mutex);
        while (!state->failed && state->status != wanted) {
            if (cancelled()) throw std::runtime_error("Network request cancelled");
            if (Clock::now() >= deadline) throw std::runtime_error("Network request timed out");
            state->ready.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(50)));
        }
        if (cancelled()) throw std::runtime_error("Network request cancelled");
        if (state->failed) throw std::runtime_error("Network request failed");
        return state->bytes;
    };
    std::string headers = value->host == "api.github.com" ? "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : "Accept: */*\r\n";
    if (!token.empty()) headers += "Authorization: Bearer " + std::string(token) + "\r\n";
    if (!body.empty()) headers += "Content-Type: application/json\r\n";
    auto wide_headers = wide(headers);
    await(WinHttpSendRequest(request.handle, wide_headers.c_str(), static_cast<DWORD>(wide_headers.size()),
          state->body.empty() ? WINHTTP_NO_REQUEST_DATA : state->body.data(), static_cast<DWORD>(state->body.size()),
          static_cast<DWORD>(state->body.size()), context), WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    reset(); await(WinHttpReceiveResponse(request.handle, nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("Invalid network response");
    HttpResponse response{static_cast<int>(status), {}, location(request.handle)};
    for (;;) {
        if (Clock::now() >= deadline) throw std::runtime_error("Network request timed out");
        reset(); auto bytes = await(WinHttpReadData(request.handle, state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr),
                                    WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        if (!bytes) return response;
        if (bytes > state->buffer.size() || bytes > limit - response.body.size()) throw HttpSizeError("Network response is too large");
        response.body.append(state->buffer.data(), bytes);
    }
}
}

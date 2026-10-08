#include "amp/artwork.hpp"
#include "amp/settings.hpp"
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <list>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace amp {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_json = 512 * 1024;
constexpr std::size_t max_file = 128 * 1024;
constexpr auto encoding_profile = "webp-768-q85-lanczos-v2";

Json parse(std::string_view text, std::size_t limit) {
    if (text.size() > limit) throw std::runtime_error("Artwork data is too large");
    return Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) throw std::runtime_error("Artwork data is too deeply nested");
        return true;
    });
}

std::string string_field(const Json& object, const char* field) {
    auto item = object.find(field);
    return item != object.end() && item->is_string() ? item->get<std::string>() : "";
}

std::wstring to_wide(std::string_view text) {
    if (text.size() > 8192) return {};
    try { return amp::wide(text); } catch (const std::exception&) { return {}; }
}

std::string to_utf8(std::wstring_view text) {
    try { return amp::utf8(text); } catch (const std::exception&) { return {}; }
}

std::string ascii_lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

struct Url { std::string host, path, query, fragment; };
std::optional<Url> url_parts(std::string_view value) {
    if (value.size() > 2048 || value.size() < 9
        || ascii_lower(std::string(value.substr(0, 8))) != "https://"
        || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; })
        || value.find('\\') != std::string_view::npos) return {};
    const auto end = value.find_first_of("/?#", 8);
    std::string authority(value.substr(8, end == std::string_view::npos ? value.size() - 8 : end - 8));
    if (authority.ends_with(":443")) authority.resize(authority.size() - 4);
    if (authority.empty() || authority.find_first_of(":@") != std::string::npos
        || !std::all_of(authority.begin(), authority.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '.' || c == '-';
        })) return {};
    Url url{ascii_lower(authority), {}, {}, {}};
    if (end == std::string_view::npos) return url;
    const auto fragment = value.find('#', end);
    const auto query = value.find('?', end);
    const auto path_end = std::min(query, fragment);
    url.path = value.substr(end, path_end == std::string_view::npos ? value.size() - end : path_end - end);
    if (query != std::string_view::npos && (fragment == std::string_view::npos || query < fragment))
        url.query = value.substr(query, fragment == std::string_view::npos ? value.size() - query : fragment - query);
    if (fragment != std::string_view::npos) url.fragment = value.substr(fragment);
    return url;
}

bool store_page(std::string_view value) {
    auto parts = url_parts(value);
    return parts && (parts->host == "music.apple.com" || parts->host == "itunes.apple.com");
}

bool animation_url(std::string_view value) {
    auto parts = url_parts(value);
    if (!parts || !parts->query.empty() || !parts->fragment.empty()) return false;
    const auto path = ascii_lower(parts->path);
    return path.ends_with(".webp") || path.ends_with(".gif") || path.ends_with(".avif");
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::string contents(max_file + 1, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    const auto count = file.gcount();
    if (file.bad() || count > static_cast<std::streamsize>(max_file)) return {};
    contents.resize(static_cast<std::size_t>(count));
    return contents;
}

std::string encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

struct Internet {
    HINTERNET handle{};
    explicit Internet(HINTERNET value) : handle(value) {
        if (!handle) throw std::runtime_error("Artwork request unavailable");
    }
    ~Internet() { WinHttpCloseHandle(handle); }
    Internet(const Internet&) = delete;
};

// The heap callback binding retains its state until HANDLE_CLOSING, including
// after cancellation. Closing a synchronous WinHTTP request from another thread
// is unsafe; asynchronous completion lets us enforce one absolute deadline.
struct RequestState {
    std::mutex mutex;
    std::condition_variable ready;
    DWORD status{}, bytes{};
    bool failed{};
    std::array<char, 8192> buffer{};
};
using Binding = std::shared_ptr<RequestState>;
void CALLBACK complete(HINTERNET, DWORD_PTR context, DWORD status, void*, DWORD bytes) {
    if (!context) return;
    auto* binding = reinterpret_cast<Binding*>(context);
    const auto state = *binding;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete binding; return; }
    {
        std::lock_guard lock(state->mutex);
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) state->failed = true;
        state->status = status;
        state->bytes = bytes;
    }
    state->ready.notify_all();
}

std::string fetch(std::string_view path) {
    Internet session(WinHttpOpen(L"AppleMusicPresence-Native/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 5000);
    Internet connection(WinHttpConnect(session.handle, L"itunes.apple.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    const auto target = to_wide(path);
    Internet request(WinHttpOpenRequest(connection.handle, L"GET", target.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
        throw std::runtime_error("Artwork request options unavailable");
    auto state = std::make_shared<RequestState>();
    if (WinHttpSetStatusCallback(request.handle, complete, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS
                                 | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
        throw std::runtime_error("Artwork request callback unavailable");
    auto binding = std::make_unique<Binding>(state);
    auto context = reinterpret_cast<DWORD_PTR>(binding.get());
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        throw std::runtime_error("Artwork request context unavailable");
    binding.release(); // The final HANDLE_CLOSING callback owns this binding.
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    auto reset = [&] { std::lock_guard lock(state->mutex); state->status = 0; state->bytes = 0; };
    auto await = [&](BOOL started, DWORD wanted) {
        if (!started && GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Artwork request failed");
        std::unique_lock lock(state->mutex);
        if (!state->ready.wait_until(lock, deadline, [&] { return state->failed || state->status == wanted; })
            || state->failed) throw std::runtime_error("Artwork request timed out or failed");
        return state->bytes;
    };
    await(WinHttpSendRequest(request.handle, L"Accept: application/json\r\n", static_cast<DWORD>(-1L),
                             WINHTTP_NO_REQUEST_DATA, 0, 0, context), WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
    reset();
    await(WinHttpReceiveResponse(request.handle, nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("Artwork catalog returned an error");
    std::string payload;
    while (true) {
        if (Clock::now() >= deadline) throw std::runtime_error("Artwork request timed out");
        reset();
        const auto bytes = await(WinHttpReadData(request.handle, state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr),
                                 WINHTTP_CALLBACK_STATUS_READ_COMPLETE);
        if (!bytes) break;
        if (bytes > state->buffer.size() || payload.size() + bytes > max_json)
            throw std::runtime_error("Artwork response is too large");
        payload.append(state->buffer.data(), bytes);
    }
    return payload;
}

std::string track_key(const Track& track) {
    return artwork_detail::normalize(track.title) + '\0' + artwork_detail::normalize(track.artist)
        + '\0' + artwork_detail::normalize(track.album);
}

bool valid_metadata(const Track& track) {
    for (const auto* value : {&track.title, &track.artist, &track.album}) {
        const auto text = to_wide(*value);
        if (text.empty() || text.size() > 512 || value->find('\0') != std::string::npos
            || artwork_detail::normalize(*value).empty()) return false;
    }
    return true;
}
}

namespace artwork_detail {
std::string normalize(std::string_view value) {
    auto text = to_wide(value);
    if (text.empty()) return {};
    int count = NormalizeString(NormalizationKC, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring normalized(count, L'\0');
    count = NormalizeString(NormalizationKC, text.data(), static_cast<int>(text.size()), normalized.data(), count);
    if (count <= 0) return {};
    normalized.resize(count);
    count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(),
                          static_cast<int>(normalized.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) return {};
    std::wstring lower(count, L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(),
                       static_cast<int>(normalized.size()), lower.data(), count, nullptr, nullptr, 0)) return {};
    std::vector<WORD> types(lower.size());
    if (!GetStringTypeW(CT_CTYPE1, lower.data(), static_cast<int>(lower.size()), types.data())) return {};
    std::wstring result;
    bool pending_space = false;
    for (std::size_t index = 0; index < lower.size(); ++index) {
        // Keep supplementary characters intact rather than discarding two
        // surrogate code units and accidentally merging different titles.
        if (lower[index] >= 0xd800 && lower[index] <= 0xdbff && index + 1 < lower.size()
            && lower[index + 1] >= 0xdc00 && lower[index + 1] <= 0xdfff) {
            if (pending_space) result += L' ';
            pending_space = false;
            result += lower[index]; result += lower[++index];
            continue;
        }
        if (!(types[index] & (C1_ALPHA | C1_DIGIT))) { pending_space = !result.empty(); continue; }
        if (pending_space) result += L' ';
        pending_space = false;
        if (lower[index] == L'\u00df') result += L"ss";
        else result += lower[index] == L'\u03c2' ? L'\u03c3' : lower[index];
    }
    return to_utf8(result);
}

std::optional<std::string> thumbnail(std::string_view value) {
    const auto url = url_parts(value);
    if (!url || !(url->host.ends_with(".mzstatic.com") || url->host.ends_with(".itunes.apple.com"))) return {};
    std::string result(value);
    if (!url->host.ends_with(".mzstatic.com") || !url->path.starts_with("/image/thumb/")
        || !url->query.empty() || !url->fragment.empty()) return result;
    static const std::regex rendition(R"(/([0-9]{1,5})x([0-9]{1,5})bb(?:-[0-9]+)?\.(jpg|jpeg|png)$)");
    std::smatch match;
    if (std::regex_search(url->path, match, rendition) && match[1] == match[2]
        && std::stoi(match[1]) < 1024) {
        const auto slash = result.rfind('/');
        result.resize(slash + 1);
        result += "1024x1024bb." + match[3].str();
    }
    return result;
}

std::optional<std::string> album_key(std::string_view page) {
    auto url = url_parts(page);
    if (!url || url->host != "music.apple.com") return {};
    static const std::regex path(R"(^/([A-Za-z]{2})/album/[^/?#]+/([0-9]{1,20})/?$)");
    std::smatch match;
    if (!std::regex_match(url->path, match, path)) return {};
    return "album:" + ascii_lower(match[1]) + ":" + match[2].str();
}

std::optional<Artwork> catalog_match(std::string_view payload, const Track& track) {
    if (!valid_metadata(track)) return {};
    const auto data = parse(payload, max_json);
    if (!data.is_object()) throw std::runtime_error("Invalid artwork response");
    const auto rows = data.find("results");
    if (rows == data.end() || !rows->is_array()) throw std::runtime_error("Invalid artwork response");
    std::vector<Artwork> matches;
    for (const auto& row : *rows) {
        if (!row.is_object() || string_field(row, "kind") != "song"
            || normalize(string_field(row, "trackName")) != normalize(track.title)
            || normalize(string_field(row, "artistName")) != normalize(track.artist)
            || normalize(string_field(row, "collectionName")) != normalize(track.album)) continue;
        const auto image = thumbnail(string_field(row, "artworkUrl100"));
        const auto page = string_field(row, "trackViewUrl");
        if (!image || !store_page(page)) continue;
        if (std::none_of(matches.begin(), matches.end(), [&](const Artwork& a) { return a.url == *image && a.track_url == page; }))
            matches.push_back(Artwork{*image, page, false});
    }
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1 && matches.size() <= 4
        && std::all_of(matches.begin(), matches.end(), [&](const Artwork& a) { return a.url == matches.front().url; }))
        return Artwork{matches.front().url, "", false};
    return {};
}

std::optional<Artwork> mapped_cover(std::string_view payload, const Track& track) {
    if (!valid_metadata(track)) return {};
    const auto data = parse(payload, max_file);
    if (!data.is_object() || !data.contains("version") || data["version"] != 1
        || !data.contains("albums") || !data["albums"].is_array() || data["albums"].size() > 256) return {};
    std::optional<Artwork> result;
    for (const auto& row : data["albums"]) {
        if (!row.is_object() || normalize(string_field(row, "artist")) != normalize(track.artist)
            || normalize(string_field(row, "album")) != normalize(track.album)) continue;
        const auto image = string_field(row, "image_url"), page = string_field(row, "album_url");
        auto url = url_parts(page);
        if (!animation_url(image) || !store_page(page) || !url->query.empty() || !url->fragment.empty()) continue;
        if (result && (result->url != image || result->track_url != page)) return {};
        result = Artwork{image, page, true};
    }
    return result;
}

std::optional<Artwork> cached_motion(std::string_view payload, std::string_view repository,
                                    std::string_view verified_page, double now) {
    const auto key = album_key(verified_page);
    static const std::regex repo(R"(^[A-Za-z0-9][A-Za-z0-9_.-]{0,99}/[A-Za-z0-9][A-Za-z0-9_.-]{0,99}$)");
    if (!key || !std::regex_match(repository.begin(), repository.end(), repo)) return {};
    const auto data = parse(payload, max_file);
    if (!data.is_object() || string_field(data, "repository") != repository
        || string_field(data, "encoding_profile") != encoding_profile || !data.contains("albums")
        || !data["albums"].is_object()) return {};
    const std::string prefix = "https://raw.githubusercontent.com/" + std::string(repository) + "/";
    static const std::regex suffix(R"(^[a-f0-9]{40}/artwork/motion/([0-9]{1,20})-[a-f0-9]{12}\.webp$)");
    unsigned examined = 0;
    for (const auto& item : data["albums"].items()) {
        if (++examined > 128) break;
        const auto& entry = item.value();
        if (!entry.is_object() || !entry.contains("cover") || !entry["cover"].is_object()
            || !entry.contains("expires") || !entry["expires"].is_number()) continue;
        const auto expires = entry["expires"].get<double>();
        if (!(expires > now && expires <= now + 8 * 86400)) continue;
        const auto& cover = entry["cover"];
        const auto page = string_field(cover, "track_url"), image = string_field(cover, "url");
        if (album_key(page) != key || !url_parts(image) || !image.starts_with(prefix)) continue;
        const auto filename = image.substr(prefix.size());
        std::smatch name;
        if (!std::regex_match(filename, name, suffix) || name[1] != key->substr(key->rfind(':') + 1)) continue;
        const auto url = url_parts(page);
        return Artwork{image, "https://music.apple.com" + url->path, true};
    }
    return {};
}
}

struct ArtworkResolver::Impl {
    Settings settings;
    std::filesystem::path data_dir;
    struct Entry { std::string key; std::optional<Artwork> artwork; Clock::time_point expires; };
    std::list<Entry> cache;
    Clock::time_point next_request{};
    explicit Impl(const Settings& value, std::filesystem::path path) : settings(value), data_dir(std::move(path)) {}
};

ArtworkResolver::ArtworkResolver(const Settings& settings, std::filesystem::path data_dir)
    : impl_(std::make_unique<Impl>(settings, std::move(data_dir))) {}
ArtworkResolver::~ArtworkResolver() = default;

std::optional<Artwork> ArtworkResolver::resolve(const Track& track) {
    if (!impl_->settings.artwork || !valid_metadata(track)) return {};
    const auto key = track_key(track);
    auto cached = std::find_if(impl_->cache.begin(), impl_->cache.end(), [&](const auto& entry) { return entry.key == key; });
    if (cached != impl_->cache.end()) {
        if (cached->expires > Clock::now()) {
            const auto artwork = cached->artwork;
            impl_->cache.splice(impl_->cache.begin(), impl_->cache, cached);
            return artwork;
        }
        impl_->cache.erase(cached);
    }
    std::optional<Artwork> result;
    auto ttl = std::chrono::seconds(600);
    try {
        if (impl_->settings.motion_artwork) {
            // Same explicit map as the Python app. It does not imply that a
            // similarly named deluxe/live/remastered edition has this cover.
            constexpr auto bundled = R"({"version":1,"albums":[{"artist":"The Weeknd","album":"After Hours","image_url":"https://raw.githubusercontent.com/jacobortiz/apple-music-presence/main/artwork/after-hours-hq.webp","album_url":"https://music.apple.com/us/album/after-hours/1499378108"}]})";
            if (auto custom = read_file(impl_->data_dir / "album_artwork.json")) {
                try { result = artwork_detail::mapped_cover(*custom, track); } catch (const std::exception&) {}
            }
            if (!result) result = artwork_detail::mapped_cover(bundled, track);
        }
        if (!result) {
            const auto country = ascii_lower(impl_->settings.country);
            if (country.size() != 2 || !std::all_of(country.begin(), country.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
                return {};
            std::this_thread::sleep_until(impl_->next_request);
            impl_->next_request = Clock::now() + std::chrono::milliseconds(3200);
            const auto path = "/search?term=" + encode(track.title + " " + track.artist + " " + track.album)
                + "&country=" + country + "&media=music&entity=song&limit=25";
            result = artwork_detail::catalog_match(fetch(path), track);
            if (result && impl_->settings.motion_artwork && !result->track_url.empty()) {
                if (const auto motion = read_file(impl_->data_dir / "motion_cache.json")) {
                    const auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                    try {
                        if (auto animated = artwork_detail::cached_motion(*motion, impl_->settings.artwork_repository, result->track_url, now))
                            result = std::move(animated);
                    } catch (const std::exception&) {} // A corrupt optional cache never hides valid static art.
                }
            }
        }
        if (result) ttl = std::chrono::seconds(86400);
    } catch (const std::exception&) {
        ttl = std::chrono::seconds(10); // Temporary failures must be retried, not cached as absent.
    }
    impl_->cache.push_front({key, result, Clock::now() + ttl});
    if (impl_->cache.size() > 128) impl_->cache.pop_back();
    return result;
}
}

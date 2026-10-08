#include "amp/artwork.hpp"
#include "amp/settings.hpp"
#include <windows.h>
#include <amp/catalog.hpp>
#include <amp/motion.hpp>
#include <amp/artwork_host.hpp>
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
    ArtworkDependencies dependencies;
    CatalogResolver catalog;
    std::function<void()> changed;
    HANDLE stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    struct Entry { std::string key; std::optional<Artwork> artwork; Clock::time_point expires; };
    std::list<Entry> cache;
    Clock::time_point next_request{};
    struct Alias { std::string key, page; Clock::time_point expires; };
    struct Motion { std::string key; std::optional<Artwork> cover; double expires; std::string message; };
    struct Job { Track track; std::string page, key; };
    std::mutex mutex;
    std::condition_variable ready;
    std::list<Alias> aliases;
    std::list<Motion> motions;
    std::optional<Job> pending, active;
    std::optional<std::string> wanted;
    std::thread worker;
    Impl(const Settings& value, std::filesystem::path path, std::function<void()> notify, ArtworkDependencies hooks)
        : settings(value), data_dir(std::move(path)), dependencies(std::move(hooks)),
          catalog(value.country, dependencies.http, dependencies.request_interval), changed(std::move(notify)) {
        if (!stop) throw std::runtime_error("Artwork cancellation unavailable");
        if (!dependencies.http) dependencies.http = http_get;
        try {
            if (settings.artwork && settings.motion_artwork && host_detail::valid_repository(settings.artwork_repository)) {
                load_motion();
                worker = std::thread([this] { run(); });
            }
        } catch (...) { CloseHandle(stop); throw; }
    }
    ~Impl() { cancel(); if (worker.joinable()) worker.join(); CloseHandle(stop); }
    bool cancelled() const { return WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }
    void cancel() noexcept { SetEvent(stop); ready.notify_all(); }
    void signal() { if (changed && !cancelled()) { try { changed(); } catch (...) {} } }
    void load_motion() {
        auto data = read_file(data_dir / "motion_cache.json");
        if (!data) return;
        try {
            const auto json = parse(*data, max_file);
            if (string_field(json, "repository") != settings.artwork_repository
                || string_field(json, "encoding_profile") != encoding_profile
                || !json.contains("albums") || !json["albums"].is_object()) return;
            static const std::regex key_pattern(R"(^album:[a-z]{2}:[0-9]{1,20}$)");
            for (const auto& [key, row] : json["albums"].items()) {
                if (motions.size() >= 128) break;
                if (!row.is_object() || !row.contains("expires") || !row["expires"].is_number()) continue;
                auto expiry = row["expires"].get<double>();
                if (!(expiry > unix_time() && expiry <= unix_time() + 8 * 86400)) continue;
                if (row.contains("cover") && row["cover"].is_null() && std::regex_match(key, key_pattern)) {
                    motions.push_back({key, {}, expiry, "No motion cover; using normal artwork"}); continue;
                }
                if (!row.contains("cover") || !row["cover"].is_object()) continue;
                const auto page = string_field(row["cover"], "track_url");
                const auto verified_key = artwork_detail::album_key(page);
                if (!verified_key) continue;
                Json one = json; one["albums"] = Json{{key, row}};
                auto cover = artwork_detail::cached_motion(one.dump(), settings.artwork_repository, page, unix_time());
                if (cover && std::none_of(motions.begin(), motions.end(), [&](const Motion& m) { return m.key == *verified_key; }))
                    motions.push_back({*verified_key, std::move(cover), expiry, "Animated cover ready"});
            }
        } catch (...) { /* Invalid optional cache cannot hide normal artwork. */ }
    }
    Json motion_data() {
        Json albums = Json::object();
        for (const auto& row : motions) {
            Json cover = row.cover ? Json{{"url", row.cover->url}, {"track_url", row.cover->track_url}} : Json(nullptr);
            albums[row.key] = Json{{"cover", std::move(cover)}, {"expires", row.expires}};
        }
        return Json{{"repository", settings.artwork_repository}, {"encoding_profile", encoding_profile}, {"albums", albums}};
    }
    void save_motion(const Json& data) {
        auto text = data.dump();
        if (text.size() > max_file || cancelled()) return;
        wchar_t temporary[MAX_PATH]{};
        try {
            std::filesystem::create_directories(data_dir);
            if (!GetTempFileNameW(data_dir.c_str(), L"amp", 0, temporary)) return;
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file << text; file.close();
            if (file && !cancelled()) MoveFileExW(temporary, (data_dir / "motion_cache.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            DeleteFileW(temporary);
        } catch (...) { if (temporary[0]) DeleteFileW(temporary); }
    }
    std::optional<Artwork> motion_for(const Track& track, bool select = false) {
        if (!worker.joinable() || cancelled()) return {};
        std::lock_guard lock(mutex);
        const auto key = track_key(track);
        if (select || !wanted) wanted = key;
        if (*wanted != key) return {};
        auto alias = std::find_if(aliases.begin(), aliases.end(), [&](const Alias& a) { return a.key == key && a.expires > Clock::now(); });
        if (alias == aliases.end()) { pending.reset(); return {}; }
        auto album = artwork_detail::album_key(alias->page);
        if (!album) return {};
        if (pending && pending->key != *album) pending.reset();
        auto row = std::find_if(motions.begin(), motions.end(), [&](const Motion& m) { return m.key == *album; });
        if (row != motions.end()) {
            if (row->expires > unix_time()) return row->cover;
            motions.erase(row);
        }
        if ((!active || active->key != *album) && (!pending || pending->key != *album)) {
            pending = Job{track, alias->page, *album}; // One active album + only the latest queued album.
            ready.notify_one();
        }
        return {};
    }
    void remember(const Track& track, const Artwork& cover) {
        if (!worker.joinable() || !catalog_detail::apple_album_page(cover.track_url)) return;
        const auto page = *catalog_detail::apple_album_page(cover.track_url);
        std::lock_guard lock(mutex);
        const auto key = track_key(track);
        aliases.remove_if([&](const Alias& a) { return a.key == key; });
        aliases.push_front({key, page, Clock::now() + std::chrono::hours(24)});
        if (aliases.size() > 128) aliases.pop_back();
    }
    std::optional<Artwork> prepare(const Job& job) {
        if (dependencies.motion) return dependencies.motion(job.track, job.page, stop);
        auto prepared = prepare_motion(job.page, job.track.album, {}, data_dir, stop);
        if (!prepared) return {};
        GithubArtworkHost host(settings.artwork_repository);
        const auto id = job.key.substr(job.key.rfind(':') + 1);
        return Artwork{host.publish(id, prepared->stream, prepared->webp, stop), job.page, true};
    }
    void run() {
        for (;;) {
            Job job;
            { std::unique_lock lock(mutex); ready.wait(lock, [&] { return cancelled() || pending.has_value(); });
              if (cancelled()) return; job = *pending; active = job; pending.reset(); }
            signal();
            std::optional<Artwork> cover;
            double ttl = 86400;
            std::string message = "No motion cover; using normal artwork";
            try {
                cover = prepare(job);
                if (cover) {
                    // Validate the persistent URL/album association, even for injected providers.
                    Json row{{"cover", {{"url", cover->url}, {"track_url", cover->track_url}}}, {"expires", unix_time() + 7 * 86400}};
                    Json data{{"repository", settings.artwork_repository}, {"encoding_profile", encoding_profile}, {"albums", {{job.key, row}}}};
                    cover = artwork_detail::cached_motion(data.dump(), settings.artwork_repository, job.page, unix_time());
                    if (!cover) throw std::runtime_error("Unverified motion publication");
                    ttl = 7 * 86400; message = "Animated cover ready";
                }
            } catch (...) {
                cover.reset(); ttl = 60;
                message = "Motion unavailable; using normal artwork and retrying in a minute";
            }
            if (cancelled()) return;
            Json data;
            { std::lock_guard lock(mutex);
              active.reset(); motions.remove_if([&](const Motion& m) { return m.key == job.key; });
              motions.push_front({job.key, std::move(cover), unix_time() + ttl, std::move(message)});
              if (motions.size() > 128) motions.pop_back(); data = motion_data(); }
            save_motion(data);
            signal();
        }
    }
};

ArtworkResolver::ArtworkResolver(const Settings& settings, std::filesystem::path data_dir,
                                 std::function<void()> changed, ArtworkDependencies dependencies)
    : impl_(std::make_unique<Impl>(settings, std::move(data_dir), std::move(changed), std::move(dependencies))) {}
ArtworkResolver::~ArtworkResolver() = default;
void ArtworkResolver::cancel() noexcept { impl_->cancel(); }
std::optional<Artwork> ArtworkResolver::refresh(const Track& track) { return impl_->motion_for(track, true); }
std::string ArtworkResolver::status(const Track& track) {
    if (!impl_->settings.motion_artwork) return {};
    std::lock_guard lock(impl_->mutex);
    auto alias = std::find_if(impl_->aliases.begin(), impl_->aliases.end(), [&](const auto& a) { return a.key == track_key(track); });
    if (alias == impl_->aliases.end()) return {};
    auto key = artwork_detail::album_key(alias->page);
    if (!key) return {};
    if ((impl_->active && impl_->active->key == *key) || (impl_->pending && impl_->pending->key == *key))
        return "Preparing motion cover; using normal artwork";
    auto row = std::find_if(impl_->motions.begin(), impl_->motions.end(), [&](const auto& m) { return m.key == *key; });
    return row == impl_->motions.end() ? "" : row->message;
}
std::optional<Artwork> ArtworkResolver::resolve(const Track& track) {
    if (!impl_->settings.artwork || !valid_metadata(track) || impl_->cancelled()) return {};
    const auto key = track_key(track);
    auto cached = std::find_if(impl_->cache.begin(), impl_->cache.end(), [&](const auto& entry) { return entry.key == key; });
    if (cached != impl_->cache.end()) {
        if (cached->expires > Clock::now()) {
            const auto artwork = cached->artwork;
            impl_->cache.splice(impl_->cache.begin(), impl_->cache, cached);
            if (artwork && !artwork->animated) { impl_->remember(track, *artwork); if (auto motion = impl_->motion_for(track)) return motion; }
            return artwork;
        }
        impl_->cache.erase(cached);
    }
    std::optional<Artwork> result;
    auto ttl = std::chrono::seconds(600);
    bool transient = false;
    if (impl_->settings.motion_artwork) {
        constexpr auto bundled = R"({"version":1,"albums":[{"artist":"The Weeknd","album":"After Hours","image_url":"https://raw.githubusercontent.com/jacobortiz/apple-music-presence/main/artwork/after-hours-hq.webp","album_url":"https://music.apple.com/us/album/after-hours/1499378108"}]})";
        if (auto custom = read_file(impl_->data_dir / "album_artwork.json")) {
            try { result = artwork_detail::mapped_cover(*custom, track); } catch (...) {}
        }
        if (!result) result = artwork_detail::mapped_cover(bundled, track);
    }
    if (!result) {
        try {
            auto wait = impl_->next_request - Clock::now();
            if (wait > Clock::duration::zero() && WaitForSingleObject(impl_->stop, static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(wait).count())) == WAIT_OBJECT_0)
                return {};
            impl_->next_request = Clock::now() + impl_->dependencies.request_interval;
            const auto url = "https://itunes.apple.com/search?term=" + encode(track.title + " " + track.artist + " " + track.album)
                + "&country=" + ascii_lower(impl_->settings.country) + "&media=music&entity=song&limit=25";
            auto response = impl_->dependencies.http(url, max_json, impl_->stop);
            if (response.status != 200) throw std::runtime_error("Artwork catalog unavailable");
            auto candidates = catalog_detail::song_candidates(response.body, track);
            result = catalog_detail::known_cover(candidates);
            if (!result) result = shared_cover(candidates, impl_->stop, impl_->dependencies.http);
        } catch (...) { transient = true; }
        if (!result && !impl_->cancelled()) {
            try { result = impl_->catalog.page_cover(track, impl_->stop); transient |= impl_->catalog.retryable(); } catch (...) { transient = true; }
        }
    }
    if (impl_->cancelled()) return {};
    if (result) ttl = std::chrono::seconds(86400);
    else if (transient) ttl = std::chrono::seconds(10);
    impl_->cache.push_front({key, result, Clock::now() + ttl});
    if (impl_->cache.size() > 128) impl_->cache.pop_back();
    if (result && !result->animated) { impl_->remember(track, *result); if (auto motion = impl_->motion_for(track)) return motion; }
    return result;
}
}

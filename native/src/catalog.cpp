#include "amp/catalog.hpp"
#include "amp/artwork.hpp"
#include "amp/settings.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <list>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace amp {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_page = 3 * 1024 * 1024;
constexpr std::size_t max_catalog = 512 * 1024;
constexpr std::size_t max_cover = 2 * 1024 * 1024;

std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
std::string field(const Json& value, const char* name, std::size_t limit = 8192) {
    if (!value.is_object()) return {};
    const auto item = value.find(name);
    if (item == value.end() || !item->is_string()) return {};
    const auto& text = item->get_ref<const std::string&>();
    return text.size() <= limit ? text : "";
}
Json parse(std::string_view text, std::size_t limit) {
    if (text.size() > limit) throw std::runtime_error("Catalog data is too large");
    return Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) throw std::runtime_error("Catalog nesting limit exceeded");
        return true;
    });
}
bool valid_text(std::string_view text) {
    try { return !text.empty() && text.size() <= 8192 && wide(text).size() <= 512
        && text.find('\0') == std::string_view::npos && !artwork_detail::normalize(text).empty(); }
    catch (const std::exception&) { return false; }
}
std::string track_key(const Track& track) {
    return artwork_detail::normalize(track.title) + '\0' + artwork_detail::normalize(track.artist)
        + '\0' + artwork_detail::normalize(track.album);
}
void cancelled(HANDLE stop) {
    if (stop && WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) throw std::runtime_error("Artwork lookup cancelled");
}
bool apple_store_url(std::string_view url) {
    if (url.size() > 2048 || url.size() < 9 || lower(std::string(url.substr(0, 8))) != "https://"
        || url.find('\\') != std::string_view::npos || std::any_of(url.begin(), url.end(),
           [](unsigned char c) { return c <= 32 || c == 127; })) return false;
    auto host = lower(std::string(url.substr(8, url.find_first_of("/?#", 8) - 8)));
    if (host.ends_with(":443")) host.resize(host.size() - 4);
    return host == "music.apple.com" || host == "itunes.apple.com";
}
void unique_add(std::vector<Artwork>& values, Artwork cover) {
    if (std::find(values.begin(), values.end(), cover) == values.end()) values.push_back(std::move(cover));
}
std::string encode(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
std::size_t find_ascii(std::string_view text, std::string_view pattern, std::size_t start) {
    const auto begin = text.begin() + static_cast<std::ptrdiff_t>(std::min(start, text.size()));
    const auto found = std::search(begin, text.end(), pattern.begin(), pattern.end(), [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == b;
    });
    return found == text.end() ? std::string_view::npos : static_cast<std::size_t>(found - text.begin());
}

struct PageItem {
    std::string kind, url, id, title, artist, image;
    bool video{}, track_number{};
};
std::vector<PageItem> page_items(std::string_view html) {
    if (html.size() > max_page) throw std::runtime_error("Public page is too large");
    std::vector<PageItem> items;
    auto walk = [&](auto&& self, const Json& value) -> void {
        if (value.is_object()) {
            const auto descriptor = value.find("contentDescriptor");
            if (descriptor != value.end() && descriptor->is_object()) {
                if (items.size() >= 4096) throw std::runtime_error("Too many public page descriptors");
                PageItem item{field(*descriptor, "kind", 16), field(*descriptor, "url", 2048), {},
                              field(value, "title"), field(value, "artistName"), {},
                              value.contains("videoArtwork"), value.contains("trackNumber")};
                const auto identifiers = descriptor->find("identifiers");
                if (identifiers != descriptor->end() && identifiers->is_object()) {
                    const auto id = identifiers->find("storeAdamID");
                    if (id != identifiers->end()) {
                        if (id->is_string()) item.id = field(*identifiers, "storeAdamID", 20);
                        else if (id->is_number_unsigned()) item.id = std::to_string(id->get<std::uint64_t>());
                        else if (id->is_number_integer()) item.id = std::to_string(id->get<std::int64_t>());
                    }
                }
                const auto art = value.find("artwork");
                if (art != value.end() && art->is_object()) {
                    const auto dictionary = art->find("dictionary");
                    if (dictionary != art->end()) item.image = field(*dictionary, "url", 2048);
                }
                items.push_back(std::move(item));
            }
            for (const auto& child : value) self(self, child);
        } else if (value.is_array()) for (const auto& child : value) self(self, child);
    };
    std::size_t position = 0, scripts = 0;
    while ((position = html.find('<', position)) != std::string_view::npos) {
        if (html.substr(position, 4) == "<!--") {
            const auto end = html.find("-->", position + 4);
            if (end == std::string_view::npos) break;
            position = end + 3; continue;
        }
        const auto name_start = position + 1;
        auto name_end = name_start;
        while (name_end < html.size() && ((html[name_end] >= 'a' && html[name_end] <= 'z')
               || (html[name_end] >= 'A' && html[name_end] <= 'Z'))) ++name_end;
        const bool script = lower(std::string(html.substr(name_start, name_end - name_start))) == "script"
            && name_end < html.size() && (html[name_end] == '>' || html[name_end] == ' ' || html[name_end] == '\t'
                                         || html[name_end] == '\r' || html[name_end] == '\n');
        char quote{};
        std::size_t start = name_end;
        for (; start < html.size(); ++start) {
            if (quote) { if (html[start] == quote) quote = 0; }
            else if (html[start] == '\'' || html[start] == '"') quote = html[start];
            else if (html[start] == '>') break;
        }
        if (start == html.size()) break;
        if (!script) { position = start + 1; continue; }
        auto end = find_ascii(html, "</script", start + 1);
        while (end != std::string_view::npos && end + 8 < html.size()
               && html[end + 8] != '>' && html[end + 8] != ' ' && html[end + 8] != '\t'
               && html[end + 8] != '\r' && html[end + 8] != '\n')
            end = find_ascii(html, "</script", end + 8);
        if (end == std::string_view::npos) break;
        const auto closing_end = html.find('>', end + 8);
        if (closing_end == std::string_view::npos) break;
        if (++scripts > 128) throw std::runtime_error("Too many public page scripts");
        std::optional<Json> data;
        try { data = parse(html.substr(start + 1, end - start - 1), max_page); }
        catch (const std::exception&) { /* Non-JSON and changed page scripts are ignored. */ }
        if (data) walk(walk, *data);
        position = closing_end + 1;
    }
    return items;
}
std::string replace_all(std::string text, std::string_view token, std::string_view replacement) {
    std::size_t offset = 0;
    while ((offset = text.find(token, offset)) != std::string::npos) {
        text.replace(offset, token.size(), replacement); offset += replacement.size();
    }
    return text;
}
}

namespace catalog_detail {
std::vector<Artwork> song_candidates(std::string_view payload, const Track& track) {
    if (!valid_text(track.title) || !valid_text(track.artist) || !valid_text(track.album)) return {};
    const auto data = parse(payload, max_catalog);
    if (!data.is_object() || !data.contains("results") || !data["results"].is_array()) return {};
    std::vector<Artwork> result;
    const auto key = track_key(track);
    for (const auto& row : data["results"]) {
        if (!row.is_object() || field(row, "kind", 16) != "song") continue;
        const Track candidate{field(row, "trackName"), field(row, "artistName"), field(row, "collectionName")};
        if (!valid_text(candidate.title) || !valid_text(candidate.artist) || !valid_text(candidate.album)
            || track_key(candidate) != key) continue;
        const auto image = artwork_detail::thumbnail(field(row, "artworkUrl100", 2048));
        const auto page = field(row, "trackViewUrl", 2048);
        if (image && apple_store_url(page)) unique_add(result, {*image, page, false});
    }
    return result;
}
std::optional<Artwork> known_cover(const std::vector<Artwork>& candidates) {
    if (candidates.size() == 1) return candidates.front();
    if (candidates.size() > 1 && candidates.size() <= 4 && std::all_of(candidates.begin(), candidates.end(),
            [&](const Artwork& art) { return art.url == candidates.front().url; }))
        return Artwork{candidates.front().url, "", false};
    return {};
}
std::optional<std::string> normalized_exif(std::string_view bytes) {
    if (bytes.size() < 14 || bytes.size() > 65533 || bytes.substr(0, 6) != std::string_view("Exif\0\0", 6)) return {};
    std::string normalized(bytes);
    const auto tiff = bytes.substr(6);
    if (tiff.substr(0, 2) != "MM" && tiff.substr(0, 2) != "II") return {};
    const bool big = tiff.substr(0, 2) == "MM";
    auto number = [&](std::size_t offset, std::size_t length) -> std::uint32_t {
        if (offset > tiff.size() || length > tiff.size() - offset) throw std::runtime_error("Invalid TIFF offset");
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < length; ++i) {
            const auto index = big ? offset + i : offset + length - 1 - i;
            value = (value << 8) | static_cast<unsigned char>(tiff[index]);
        }
        return value;
    };
    std::vector<std::pair<std::size_t, std::size_t>> regions{{0, 8}}, comments;
    auto reserve = [&](std::size_t offset, std::size_t length) {
        if (offset < 8 || !length || offset > tiff.size() || length > tiff.size() - offset)
            throw std::runtime_error("Invalid TIFF region");
        if (std::any_of(regions.begin(), regions.end(), [&](auto region) {
            return offset < region.second && region.first < offset + length;
        })) throw std::runtime_error("Overlapping TIFF regions");
        regions.emplace_back(offset, offset + length);
    };
    auto directory = [&](auto&& self, std::size_t offset, bool root) -> void {
        const auto count = number(offset, 2);
        if (!count || count > 16) throw std::runtime_error("Unknown TIFF directory");
        reserve(offset, 2 + 12 * count + 4);
        if (number(offset + 2 + 12 * count, 4)) throw std::runtime_error("Additional TIFF directories");
        std::set<std::uint32_t> tags;
        std::optional<std::uint32_t> child;
        for (std::size_t index = 0; index < count; ++index) {
            const auto entry = offset + 2 + 12 * index;
            const auto tag = number(entry, 2), kind = number(entry + 2, 2), length = number(entry + 4, 4);
            if (!tags.insert(tag).second) throw std::runtime_error("Duplicate TIFF tag");
            if (root && tag == 34665 && kind == 4 && length == 1) child = number(entry + 8, 4);
            else if (root && tag == 274 && kind == 3 && length == 1) {
                const auto orientation = number(entry + 8, 2);
                if (!orientation || orientation > 8) throw std::runtime_error("Invalid EXIF orientation");
            } else if (!root && tag == 37510 && kind == 7 && length >= 8 && length <= 4096) {
                const auto start = number(entry + 8, 4);
                reserve(start, length); comments.emplace_back(start, length);
            } else if (!root && tag == 40961 && kind == 3 && length == 1) {}
            else if (!root && (tag == 40962 || tag == 40963) && (kind == 3 || kind == 4) && length == 1) {}
            else throw std::runtime_error("Unknown EXIF layout");
        }
        if (root && child) self(self, *child, false);
    };
    try {
        if (number(2, 2) != 42) return {};
        directory(directory, number(4, 4), true);
        if (comments.size() != 1) return {};
        const auto [offset, length] = comments.front();
        std::fill_n(normalized.begin() + static_cast<std::ptrdiff_t>(offset + 6), length, '\0');
        return normalized;
    } catch (const std::exception&) { return {}; }
}
std::optional<std::string> comparable_jpeg(std::string_view bytes) {
    if (bytes.size() > max_cover || bytes.size() < 4 || bytes.substr(0, 2) != std::string_view("\xff\xd8", 2)
        || bytes.substr(bytes.size() - 2) != std::string_view("\xff\xd9", 2)) return {};
    std::string normalized(bytes);
    std::size_t offset = 2;
    bool frame = false, exif_seen = false;
    auto byte = [&](std::size_t at) { return static_cast<unsigned char>(bytes[at]); };
    while (offset + 4 <= bytes.size()) {
        if (byte(offset) != 255) return {};
        const auto marker = byte(offset + 1);
        const auto size = (byte(offset + 2) << 8) | byte(offset + 3);
        const auto end = offset + 2 + size;
        if (size < 2 || end > bytes.size()) return {};
        const auto payload = bytes.substr(offset + 4, size - 2);
        if (marker == 225 && payload.starts_with(std::string_view("Exif\0\0", 6))) {
            if (exif_seen) return {};
            exif_seen = true;
            auto exif = normalized_exif(payload);
            if (!exif) return {};
            normalized.replace(offset + 4, payload.size(), *exif);
        } else if (marker == 192 || marker == 194) {
            if (payload.size() < 6) return {};
            const auto height = (static_cast<unsigned char>(payload[1]) << 8) | static_cast<unsigned char>(payload[2]);
            const auto width = (static_cast<unsigned char>(payload[3]) << 8) | static_cast<unsigned char>(payload[4]);
            const auto components = static_cast<unsigned char>(payload[5]);
            if (!components || components > 4 || size != 8 + 3 * components || !height || height > 4096 || !width || width > 4096)
                return {};
            frame = true;
        } else if (marker == 218) {
            if (!frame || end >= bytes.size() - 2) return {};
            return normalized;
        } else if (marker != 196 && marker != 219 && marker != 221 && marker != 254 && !(marker >= 224 && marker <= 239))
            return {};
        offset = end;
    }
    return {};
}
std::optional<std::string> apple_album_page(std::string_view url) {
    if (!artwork_detail::album_key(url)) return {};
    const auto start = url.find('/', 8);
    if (start == std::string_view::npos) return {};
    const auto end = url.find_first_of("?#", start);
    return "https://music.apple.com" + std::string(url.substr(start, end - start));
}
std::optional<std::string> album_redirect(std::string_view page, std::string_view location) {
    const auto source = apple_album_page(page);
    if (!source || location.empty()) return {};
    std::string target(location);
    if (location.starts_with("//")) target = "https:" + target;
    else if (location.starts_with('/')) target = "https://music.apple.com" + target;
    else if (location.front() == '?' || location.front() == '#') target = *source + target;
    else if (location.find("://") == std::string_view::npos) target = source->substr(0, source->rfind('/') + 1) + target;
    const auto verified = apple_album_page(target);
    return verified && artwork_detail::album_key(*source) == artwork_detail::album_key(*verified) ? verified : std::nullopt;
}
std::vector<std::string> candidate_pages(std::string_view html, std::string_view title) {
    if (!valid_text(title)) return {};
    std::vector<std::string> pages, identities;
    for (const auto& item : page_items(html)) {
        if (item.kind != "song" || !valid_text(item.title)
            || artwork_detail::normalize(item.title) != artwork_detail::normalize(title)) continue;
        const auto page = apple_album_page(item.url);
        if (!page) continue;
        const auto id = *artwork_detail::album_key(*page);
        if (std::find(identities.begin(), identities.end(), id) == identities.end()) {
            identities.push_back(id); pages.push_back(*page);
            if (pages.size() > 3) return {};
        }
    }
    return pages;
}
std::optional<Artwork> album_cover(std::string_view html, std::string_view page, const Track& track) {
    const auto identity = artwork_detail::album_key(page);
    if (!identity || !valid_text(track.title) || !valid_text(track.artist) || !valid_text(track.album)) return {};
    const auto id = identity->substr(identity->rfind(':') + 1);
    const auto items = page_items(html);
    std::vector<Artwork> covers;
    for (const auto& item : items) {
        if (item.kind != "album" || !item.video || item.id != id || !valid_text(item.title)
            || artwork_detail::normalize(item.title) != artwork_detail::normalize(track.album)
            || artwork_detail::album_key(item.url) != identity) continue;
        auto image = replace_all(replace_all(replace_all(item.image, "{w}", "1024"), "{h}", "1024"), "{f}", "jpg");
        if (image.find_first_of("{}") != std::string::npos) continue;
        const auto safe_image = artwork_detail::thumbnail(image), header_page = apple_album_page(item.url);
        if (safe_image && header_page) unique_add(covers, {*safe_image, *header_page, false});
    }
    if (covers.size() != 1) return {};
    for (const auto& item : items) {
        if (item.kind == "song" && item.track_number && artwork_detail::album_key(item.url) == identity
            && valid_text(item.title) && valid_text(item.artist)
            && artwork_detail::normalize(item.title) == artwork_detail::normalize(track.title)
            && artwork_detail::normalize(item.artist) == artwork_detail::normalize(track.artist)) return covers.front();
    }
    return {};
}
}

std::optional<Artwork> shared_cover(const std::vector<Artwork>& candidates, HANDLE stop, CatalogHttpGetter getter) {
    if (candidates.size() < 2 || candidates.size() > 4) return {};
    std::set<std::string> urls;
    for (const auto& candidate : candidates) urls.insert(candidate.url);
    if (urls.size() < 2 || urls.size() > 4) return {};
    if (!getter) getter = http_get;
    std::optional<std::string> fingerprint;
    for (const auto& url : urls) {
        cancelled(stop);
        if (!artwork_detail::thumbnail(url) || url.find_first_of("?#") != std::string::npos) return {};
        const auto response = getter(url, max_cover, stop);
        cancelled(stop);
        if (response.status != 200 || response.body.size() > max_cover) throw std::runtime_error("Cover comparison unavailable");
        auto image = catalog_detail::comparable_jpeg(response.body);
        if (!image || (fingerprint && *fingerprint != *image)) return {};
        fingerprint = std::move(image);
    }
    return Artwork{*urls.begin(), "", false};
}

struct CatalogResolver::Impl {
    std::string country;
    CatalogHttpGetter getter;
    std::chrono::milliseconds interval;
    Clock::time_point next_request{};
    struct Entry { std::string key; std::optional<Artwork> cover; Clock::time_point expires; bool transient{}; };
    std::list<Entry> cache;
    bool last_retryable{};
    Impl(std::string value, CatalogHttpGetter fetcher, std::chrono::milliseconds delay)
        : country(lower(std::move(value))), getter(std::move(fetcher)), interval(delay) {
        if (country.size() != 2 || !std::all_of(country.begin(), country.end(), [](char c) { return c >= 'a' && c <= 'z'; })
            || interval.count() < 0 || interval > std::chrono::minutes(1)) throw std::runtime_error("Invalid public catalog configuration");
        if (!getter) getter = http_get;
    }
    std::string page(std::string url, HANDLE stop) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            cancelled(stop);
            const auto now = Clock::now();
            if (next_request > now) {
                const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next_request - now)
                    + std::chrono::milliseconds(1);
                if (stop) {
                    if (WaitForSingleObject(stop, static_cast<DWORD>(delay.count())) != WAIT_TIMEOUT)
                        throw std::runtime_error("Artwork lookup cancelled");
                } else std::this_thread::sleep_until(next_request);
            }
            cancelled(stop);
            next_request = Clock::now() + interval;
            auto response = getter(url, max_page, stop);
            cancelled(stop);
            if (response.body.size() > max_page) throw std::runtime_error("Public page is too large");
            if (response.status == 200) return std::move(response.body);
            const bool moved = response.status == 301 || response.status == 302 || response.status == 307 || response.status == 308;
            const auto target = moved ? catalog_detail::album_redirect(url, response.location) : std::nullopt;
            if (attempt == 2 || !target) throw std::runtime_error("Public catalog page unavailable");
            url = *target;
        }
        throw std::runtime_error("Public catalog page unavailable");
    }
};
CatalogResolver::CatalogResolver(std::string country, CatalogHttpGetter getter, std::chrono::milliseconds interval)
    : impl_(std::make_unique<Impl>(std::move(country), std::move(getter), interval)) {}
CatalogResolver::~CatalogResolver() = default;
bool CatalogResolver::retryable() const noexcept { return impl_->last_retryable; }
std::optional<Artwork> CatalogResolver::page_cover(const Track& track, HANDLE stop) {
    impl_->last_retryable = false;
    cancelled(stop);
    if (!valid_text(track.title) || !valid_text(track.artist) || !valid_text(track.album)) return {};
    const auto key = track_key(track);
    auto cached = std::find_if(impl_->cache.begin(), impl_->cache.end(), [&](const auto& entry) { return entry.key == key; });
    if (cached != impl_->cache.end()) {
        if (cached->expires > Clock::now()) {
            const auto result = cached->cover;
            impl_->last_retryable = cached->transient;
            impl_->cache.splice(impl_->cache.begin(), impl_->cache, cached);
            return result;
        }
        impl_->cache.erase(cached);
    }
    std::optional<Artwork> result;
    auto ttl = std::chrono::seconds(600);
    try {
        const auto search = impl_->page("https://music.apple.com/" + impl_->country + "/search?term="
            + encode(track.title + " " + track.artist + " " + track.album), stop);
        std::vector<Artwork> matches;
        for (const auto& page : catalog_detail::candidate_pages(search, track.title)) {
            if (!artwork_detail::album_key(page)->starts_with("album:" + impl_->country + ":")) continue;
            const auto cover = catalog_detail::album_cover(impl_->page(page, stop), page, track);
            if (cover) unique_add(matches, *cover);
        }
        // Different verified album IDs remain ambiguous, including when their
        // covers match. Do not choose an album ID for automatic motion lookup.
        if (matches.size() == 1) { result = matches.front(); ttl = std::chrono::seconds(86400); }
    } catch (const std::exception&) {
        cancelled(stop); // Cancellation propagates and never poisons the cache.
        impl_->last_retryable = true;
        ttl = std::chrono::seconds(10);
    }
    impl_->cache.push_front({key, result, Clock::now() + ttl, impl_->last_retryable});
    if (impl_->cache.size() > 128) impl_->cache.pop_back();
    return result;
}
}

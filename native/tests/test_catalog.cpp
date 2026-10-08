#include "amp/catalog.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using Json = nlohmann::json;
const std::string page = "https://music.apple.com/us/album/data/123";
const std::string other_page = "https://music.apple.com/us/album/data/999";
const std::string image = "https://is1-ssl.mzstatic.com/image/cover/{w}x{h}bb.{f}";
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json descriptor(const std::string& kind, const std::string& url = page, const std::string& id = "123") {
    return {{"kind", kind}, {"url", url}, {"identifiers", {{"storeAdamID", id}}}};
}
Json header(const std::string& url = page, const std::string& id = "123", const std::string& art = image) {
    return {{"title", "DATA"}, {"contentDescriptor", descriptor("album", url, id)},
            {"videoArtwork", nullptr}, {"artwork", {{"dictionary", {{"url", art}}}}}};
}
Json song(const std::string& title = "First", const std::string& artist = "Tainy & Guest", const std::string& url = page) {
    return {{"title", title}, {"artistName", artist}, {"trackNumber", 1},
            {"contentDescriptor", descriptor("song", url + "?i=456")}};
}
std::string html(const Json& items) {
    return "<script type=\"application/json\">" + Json{{"items", items}}.dump() + "</script>";
}
std::string integer(std::uint32_t value, unsigned width, bool big = true) {
    std::string result(width, '\0');
    for (unsigned i = 0; i < width; ++i) result[big ? width - 1 - i : i] = static_cast<char>((value >> (8 * i)) & 255);
    return result;
}
std::string exif(char letter = 'a', int orientation = 0, bool big = true) {
    const auto root_count = orientation ? 2u : 1u;
    const auto child_offset = 8 + 2 + 12 * root_count + 4;
    const auto comment_offset = child_offset + 2 + 4 * 12 + 4;
    auto pack = [&](std::uint32_t value, unsigned width) { return integer(value, width, big); };
    auto entry = [&](std::uint32_t tag, std::uint32_t kind, std::uint32_t count, std::uint32_t value) {
        return pack(tag, 2) + pack(kind, 2) + pack(count, 4) + pack(value, 4);
    };
    auto root = entry(34665, 4, 1, child_offset);
    if (orientation) root += pack(274, 2) + pack(3, 2) + pack(1, 4) + pack(orientation, 2) + std::string(2, '\0');
    const auto child = entry(37510, 7, 34, comment_offset)
        + pack(40961, 2) + pack(3, 2) + pack(1, 4) + pack(1, 2) + std::string(2, '\0')
        + entry(40962, 4, 1, 1024) + entry(40963, 4, 1, 1024);
    return std::string("Exif\0\0", 6) + (big ? "MM" : "II") + pack(42, 2) + pack(8, 4)
        + pack(root_count, 2) + root + pack(0, 4) + pack(4, 2) + child + pack(0, 4)
        + std::string("ASCII\0\0\0", 8) + std::string(26, letter);
}
std::string segment(unsigned marker, const std::string& bytes) {
    return std::string(1, '\xff') + static_cast<char>(marker) + integer(static_cast<std::uint32_t>(bytes.size() + 2), 2) + bytes;
}
std::string jpeg(char letter = 'a', int orientation = 0, const std::string& scan = "image-data",
                 const std::string& icc = std::string("ICC_PROFILE\0", 12), const std::string& metadata = "") {
    const std::string frame("\x08\x04\x00\x04\x00\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00", 15);
    const std::string start("\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00", 10);
    return std::string("\xff\xd8", 2) + segment(225, metadata.empty() ? exif(letter, orientation) : metadata)
        + segment(226, icc) + segment(192, frame) + segment(218, start) + scan + std::string("\xff\xd9", 2);
}
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { if (!handle) throw std::runtime_error("Could not create cancellation fixture"); }
    ~Event() { CloseHandle(handle); }
};
}

void test_catalog() {
    using namespace amp;
    using namespace amp::catalog_detail;
    const Track track{"First", "Tainy & Guest", "DATA"};
    check(apple_album_page(page + "?i=1#fragment") == page, "Canonical public album page strips track/query fragments");
    check(!apple_album_page("https://user:secret@music.apple.com/us/album/data/123"), "Reject credentials in album page");
    check(!apple_album_page("https://music.apple.com.evil.test/us/album/data/123"), "Reject false Apple hostname");
    check(album_redirect("https://music.apple.com/us/album/old/123", "/us/album/data/123") == page,
          "Accept canonical slug redirect for same album and storefront");
    for (const auto& target : {other_page, std::string("https://music.apple.com/gb/album/data/123"),
                              std::string("https://evil.test/us/album/data/123")})
        check(!album_redirect(page, target), "Reject redirects to different identity or host");
    check(candidate_pages(html(Json::array({song()})), "First") == std::vector<std::string>{page}, "Extract exact song album candidate");
    check(candidate_pages(html(Json::array({song("First (Remix)")})), "First").empty(), "Preserve remix title while finding pages");
    check(candidate_pages(html(Json::array({song(), song("First", "Other", page + "?extra=1")})), "First").size() == 1,
          "Deduplicate album identity without selecting a different release");
    Json too_many = Json::array();
    for (unsigned i = 0; i < 4; ++i) too_many.push_back(song("First", "Tainy", "https://music.apple.com/us/album/data/" + std::to_string(i)));
    check(candidate_pages(html(too_many), "First").empty(), "Never examine only a subset of excessive album candidates");
    check(candidate_pages("<!--" + html(Json::array({song()})) + "-->", "First").empty(), "Ignore commented-out scripts");
    check(candidate_pages("<div data-text='" + html(Json::array({song()})) + "'>ignored</div>", "First").empty(),
          "Ignore script-like text inside HTML attributes");
    check(candidate_pages("<SCRIPT data-label='>'>" + Json{{"items", Json::array({song()})}}.dump() + "</SCRIPT>", "First").size() == 1,
          "Parse quoted tag attributes and uppercase script tags");
    const auto album = html(Json::array({header(), song(), song("Second", "Tainy & Another")}));
    auto cover = album_cover(album, page, track);
    check(cover && cover->url == "https://is1-ssl.mzstatic.com/image/cover/1024x1024bb.jpg" && cover->track_url == page,
          "Verify own header and exact track before selecting public cover");
    check(!album_cover(album, page, {"First", "Different", "DATA"}), "Reject wrong track artist");
    check(!album_cover(album, page, {"First", "Tainy & Guest", "DATA (Instrumental)"}), "Reject wrong album edition");
    check(!album_cover(html(Json::array({header(page, "999"), song()})), page, track), "Verify header storeAdamID");
    check(!album_cover(html(Json::array({header(), song("First", "Tainy & Guest", other_page)})), page, track), "Reject recommended song from another album");
    check(!album_cover(html(Json::array({header(), header(page, "123", image + "other_page"), song()})), page, track), "Require unique album header cover");
    auto no_video = header(); no_video.erase("videoArtwork");
    check(!album_cover(html(Json::array({no_video, song()})), page, track), "Fail closed on changed header structure");
    check(!album_cover(html(Json::array({header(page, "123", "https://evil.test/{w}.jpg"), song()})), page, track), "Require Apple CDN header artwork");
    bool rejected = false;
    try { candidate_pages(std::string(3 * 1024 * 1024 + 1, ' '), "First"); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Bound public page bytes");

    Json result{{"kind", "song"}, {"trackName", "First"}, {"artistName", "Tainy & Guest"}, {"collectionName", "DATA"},
                {"artworkUrl100", "https://is1-ssl.mzstatic.com/image/thumb/first/100x100bb.jpg"}, {"trackViewUrl", page + "?i=1"}};
    auto second = result; second["trackViewUrl"] = other_page;
    auto candidates = song_candidates(Json{{"results", Json::array({result, second})}}.dump(), track);
    check(candidates.size() == 2 && known_cover(candidates)->track_url.empty(), "Identical catalog cover uses no guessed album link");
    second["artworkUrl100"] = "https://is1-ssl.mzstatic.com/image/thumb/second/100x100bb.jpg";
    candidates = song_candidates(Json{{"results", Json::array({result, second})}}.dump(), track);
    check(candidates.size() == 2 && !known_cover(candidates), "Retain distinct covers for strict display comparison");
    second["collectionName"] = "DATA (Deluxe)";
    check(song_candidates(Json{{"results", Json::array({second})}}.dump(), track).empty(), "Keep full edition words in catalog extraction");
    check(song_candidates("{\"results\":[null,42,{}]}", track).empty(), "Ignore malformed catalog rows");

    Json album_result{{"collectionType", "Album"}, {"artistName", "Tainy & Guest"}, {"collectionName", "DATA"},
        {"artworkUrl100", "https://is1-ssl.mzstatic.com/image/thumb/album/100x100bb-75.jpg"}, {"collectionViewUrl", page}};
    auto albums = album_candidates(Json{{"results", Json::array({album_result})}}.dump(), track.artist, track.album);
    auto album_match = known_cover(albums);
    check(album_match && album_match->url == "https://is1-ssl.mzstatic.com/image/thumb/album/1024x1024bb.jpg"
          && album_match->track_url == page, "Exact album-only match supplies sharp art and verified collection page");
    check(album_candidates(Json{{"results", Json::array({album_result})}}.dump(), "Other Artist", track.album).empty(),
          "Album fallback never substitutes a different artist");
    for (const auto& edition : {"DATA (Deluxe)", "DATA (Instrumental)", "DATA (Live)", "DATA (Remastered)"})
        check(album_candidates(Json{{"results", Json::array({album_result})}}.dump(), track.artist, edition).empty(),
              "Album fallback preserves complete edition words");
    auto alternate_album = album_result; alternate_album["collectionViewUrl"] = other_page;
    albums = album_candidates(Json{{"results", Json::array({album_result, alternate_album})}}.dump(), track.artist, track.album);
    album_match = known_cover(albums);
    check(albums.size() == 2 && album_match && album_match->track_url.empty(),
          "Identical album covers across releases never choose an arbitrary album ID");
    alternate_album["artworkUrl100"] = "https://is1-ssl.mzstatic.com/image/thumb/different-album/100x100bb.jpg";
    albums = album_candidates(Json{{"results", Json::array({album_result, alternate_album})}}.dump(), track.artist, track.album);
    check(albums.size() == 2 && !known_cover(albums), "Different album covers remain ambiguous until strict display comparison");
    unsigned album_downloads = 0;
    const auto shared_album = shared_cover(albums, nullptr, [&](std::string_view, std::size_t, HANDLE) {
        return HttpResponse{200, jpeg(++album_downloads == 1 ? 'a' : 'b'), ""};
    });
    check(shared_album && shared_album->track_url.empty() && album_downloads == 2,
          "Album fallback reuses exact duplicate-image comparison without guessing a release");
    for (const auto* bad_field : {"collectionType", "artistName", "collectionName", "artworkUrl100", "collectionViewUrl"}) {
        auto invalid_album = album_result; invalid_album[bad_field] = 42;
        check(album_candidates(Json{{"results", Json::array({invalid_album})}}.dump(), track.artist, track.album).empty(),
              "Reject malformed album metadata and URL field types");
    }
    for (const auto& invalid_url : {"http://is1-ssl.mzstatic.com/cover.jpg", "https://is1-ssl.mzstatic.com.evil.test/cover.jpg",
                                   "https://user:secret@is1-ssl.mzstatic.com/cover.jpg"}) {
        auto invalid_album = album_result; invalid_album["artworkUrl100"] = invalid_url;
        check(album_candidates(Json{{"results", Json::array({invalid_album})}}.dump(), track.artist, track.album).empty(),
              "Reject unsafe album artwork destinations");
    }
    auto invalid_album = album_result; invalid_album["collectionViewUrl"] = "https://evil.test/album/123";
    check(album_candidates(Json{{"results", Json::array({invalid_album})}}.dump(), track.artist, track.album).empty(),
          "Require public Apple store page for album fallback");
    check(album_candidates("{\"results\":[null,42,{}]}", track.artist, track.album).empty(), "Ignore malformed album result rows");
    for (const auto& metadata : std::vector<std::pair<std::string, std::string>>{
            {"", "DATA"}, {"Artist", ""}, {"!", "DATA"}, {std::string(513, 'a'), "DATA"}})
        check(album_candidates("not JSON: must never be parsed for incomplete metadata", metadata.first, metadata.second).empty(),
              "Skip incomplete album metadata before parsing or network work");

    for (bool big : {true, false}) {
        const auto original = exif('a', 0, big);
        const auto normalized = normalized_exif(original);
        check(normalized && normalized == normalized_exif(exif('b', 0, big)), "Normalize only known UserComment for both TIFF byte orders");
        check(normalized->substr(0, normalized->size() - 34) == original.substr(0, original.size() - 34)
              && normalized->substr(normalized->size() - 34) == std::string(34, '\0'), "Preserve all other EXIF bytes");
    }
    check(comparable_jpeg(jpeg('a')) == comparable_jpeg(jpeg('b')), "Identical visible JPEG differing only in UserComment shares cover");
    check(comparable_jpeg(jpeg('a', 1)) != comparable_jpeg(jpeg('a', 6)), "Keep orientation differences");
    check(comparable_jpeg(jpeg('a', 0, "different pixels")) != comparable_jpeg(jpeg()), "Keep compressed image differences");
    check(comparable_jpeg(jpeg('a', 0, "image-data", "different color profile")) != comparable_jpeg(jpeg()), "Keep color profile differences");
    for (const auto [offset, size, value] : std::vector<std::tuple<unsigned, unsigned, unsigned>>{
            {24, 4, 8}, {28, 4, 26}, {38, 4, 5000}, {42, 4, 26}, {46, 2, 34853}}) {
        auto broken = exif(); broken.replace(offset, size, integer(value, size));
        check(!normalized_exif(broken) && !comparable_jpeg(jpeg('a', 0, "image-data", "ICC", broken)), "Reject unknown or overlapping EXIF structures");
    }
    auto truncated = exif(); truncated.pop_back();
    check(!normalized_exif(truncated), "Reject truncated EXIF");
    auto invalid = jpeg(); invalid.replace(4, 2, std::string("\0\1", 2));
    check(!comparable_jpeg(invalid), "Reject invalid JPEG segment length");
    check(!comparable_jpeg(std::string(2 * 1024 * 1024 + 1, 'x')), "Bound JPEG comparison input");
    unsigned downloads = 0;
    cover = shared_cover(candidates, nullptr, [&](std::string_view, std::size_t limit, HANDLE) {
        check(limit == 2 * 1024 * 1024, "Limit comparison downloads to two MiB");
        return HttpResponse{200, jpeg(++downloads == 1 ? 'a' : 'b'), ""};
    });
    check(cover && cover->track_url.empty() && downloads == 2, "Verify duplicate display bytes without choosing album ID");
    Event stopped;
    SetEvent(stopped.handle); downloads = 0; rejected = false;
    try { shared_cover(candidates, stopped.handle, [&](std::string_view, std::size_t, HANDLE) { ++downloads; return HttpResponse{}; }); }
    catch (const std::exception&) { rejected = true; }
    check(rejected && downloads == 0, "Stop before comparison network when cancelled");

    using namespace std::chrono_literals;
    unsigned requests = 0;
    std::vector<std::string> responses{html(Json::array({song()})), album,
        html(Json::array({song("Second", "Tainy & Another")})), album};
    CatalogResolver resolver("US", [&](std::string_view, std::size_t limit, HANDLE) {
        check(limit == 3 * 1024 * 1024 && requests < responses.size(), "Bound public page requests");
        return HttpResponse{200, responses[requests++], ""};
    }, 0ms);
    check(resolver.page_cover(track).has_value(), "Resolve recording missing from catalog using verified public page");
    check(resolver.page_cover({"Second", "Tainy & Another", "DATA"}).has_value() && requests == 4,
          "Verify each requested song independently instead of seeding album rows");
    check(resolver.page_cover(track).has_value() && requests == 4, "Reuse verified positive track cache");

    auto second_header = header(other_page, "999", "https://is1-ssl.mzstatic.com/image/other_page/{w}x{h}bb.{f}");
    responses = {html(Json::array({song()})), html(Json::array({header(), song(), song("Shared")})),
        html(Json::array({song("Shared"), song("Shared", "Tainy & Guest", other_page)})),
        html(Json::array({header(), song("Shared")})), html(Json::array({second_header, song("Shared", "Tainy & Guest", other_page)}))};
    requests = 0;
    CatalogResolver ambiguity("US", [&](std::string_view, std::size_t, HANDLE) {
        check(requests < responses.size(), "Bound ambiguity fixture requests"); return HttpResponse{200, responses[requests++], ""};
    }, 0ms);
    check(ambiguity.page_cover(track).has_value(), "Resolve unique first song on an album");
    check(!ambiguity.page_cover({"Shared", "Tainy & Guest", "DATA"}) && requests == 5,
          "Earlier album lookup cannot hide ambiguity on a song shared across releases");
    check(!ambiguity.page_cover({"Shared", "Tainy & Guest", "DATA"}) && requests == 5, "Cache confirmed page miss");

    requests = 0;
    const std::string alias = "https://music.apple.com/us/album/old/123";
    CatalogResolver redirected("US", [&](std::string_view url, std::size_t, HANDLE) {
        ++requests;
        if (url.find("/search?") != std::string_view::npos) return HttpResponse{200, html(Json::array({song("First", "Tainy & Guest", alias)})), ""};
        if (url == alias) return HttpResponse{301, "", page};
        check(url == page, "Only follow verified canonical album redirect"); return HttpResponse{200, album, ""};
    }, 0ms);
    check(redirected.page_cover(track).has_value() && requests == 3, "Follow same-ID slug redirect and verify returned page");
    requests = 0;
    CatalogResolver failure("US", [&](std::string_view, std::size_t, HANDLE) -> HttpResponse {
        ++requests; throw std::runtime_error("Offline network fixture failure");
    }, 0ms);
    check(!failure.page_cover(track) && failure.retryable(), "Surface new transient page failure to the enclosing artwork cache");
    check(!failure.page_cover(track) && failure.retryable() && requests == 1, "Cached transient page failure remains retryable");
    check(!failure.page_cover({"", "Artist", "Album"}) && requests == 1, "Never search incomplete metadata");
    check(!failure.retryable(), "Invalid metadata does not inherit an earlier temporary-failure flag");
    CatalogResolver missing("US", [&](std::string_view, std::size_t, HANDLE) {
        return HttpResponse{200, html(Json::array()), ""};
    }, 0ms);
    check(!missing.page_cover(track) && !missing.retryable(), "Confirmed missing public recording is not marked transient");
    check(!missing.page_cover(track) && !missing.retryable(), "Cached confirmed miss keeps its ordinary lifetime");
    ResetEvent(stopped.handle); requests = 0;
    CatalogResolver cancellation("US", [&](std::string_view, std::size_t, HANDLE stop) {
        ++requests; SetEvent(stop); return HttpResponse{200, "", ""};
    }, 0ms);
    rejected = false;
    try { cancellation.page_cover(track, stopped.handle); } catch (const std::exception&) { rejected = true; }
    check(rejected && requests == 1, "Propagate cancellation without caching a failure");
}

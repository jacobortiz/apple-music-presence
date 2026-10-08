#include <amp/artwork.hpp>
#include <amp/presence.hpp>
#include <amp/settings.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("amp-album-fallback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { require(std::filesystem::create_directory(path), "Create isolated album fallback fixture"); }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove(path / "motion_cache.json", ignored);
        std::filesystem::remove(path, ignored);
    }
};
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { require(handle != nullptr, "Create album cancellation fixture"); }
    ~Event() { CloseHandle(handle); }
};
const amp::Track edited{"Song (My Edit)", "Example Artist", "Example Album"};
const std::string cover_page = "https://music.apple.com/us/album/example/123";
const std::string fallback_page = "https://music.apple.com/us/album/example/321";
const std::string thumbnail = "https://is1-ssl.mzstatic.com/image/thumb/album/100x100bb.jpg";
amp::Settings settings(bool motion = false) {
    amp::Settings value; value.artwork = true; value.motion_artwork = motion;
    if (motion) value.artwork_repository = "example/public-artwork";
    return value;
}
amp::ArtworkDependencies hooks() {
    amp::ArtworkDependencies value; value.request_interval = 0ms; return value;
}
Json album_row(const std::string& artist = edited.artist, const std::string& album = edited.album,
               const std::string& page = cover_page, const std::string& image = thumbnail) {
    return {{"collectionType", "Album"}, {"artistName", artist}, {"collectionName", album},
        {"artworkUrl100", image}, {"collectionViewUrl", page}};
}
amp::HttpResponse results(Json rows) { return {200, Json{{"results", std::move(rows)}}.dump(), ""}; }
std::string html(const Json& items) { return "<script>" + Json{{"items", items}}.dump() + "</script>"; }
Json public_song(const amp::Track& track) {
    return {{"title", track.title}, {"artistName", track.artist}, {"trackNumber", 1},
        {"contentDescriptor", {{"kind", "song"}, {"url", fallback_page + "?i=456"}}}};
}
std::string public_album(const amp::Track& track) {
    Json header{{"title", track.album}, {"videoArtwork", nullptr},
        {"contentDescriptor", {{"kind", "album"}, {"url", fallback_page}, {"identifiers", {{"storeAdamID", "321"}}}}},
        {"artwork", {{"dictionary", {{"url", "https://is1-ssl.mzstatic.com/image/public/{w}x{h}bb.{f}"}}}}}};
    return html(Json::array({header, public_song(track)}));
}
bool song_request(std::string_view url) { return url.starts_with("https://itunes.apple.com/search?") && url.find("&entity=song&") != url.npos; }
bool album_request(std::string_view url) { return url.starts_with("https://itunes.apple.com/search?") && url.find("&entity=album&") != url.npos; }
bool page_search(std::string_view url) { return url.starts_with("https://music.apple.com/us/search?"); }
std::string different_jpeg(char scan) {
    // Structurally valid bounded JPEG headers; the two scan bodies differ.
    return std::string("\xff\xd8\xff\xc0\x00\x0b", 6)
        + std::string("\x08\x00\x01\x00\x01\x01\x01\x11\x00", 9)
        + std::string("\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00", 10)
        + scan + std::string("\xff\xd9", 2);
}
}

void test_album_fallback() {
    // An edited local title can miss a song while the exact artist/album remain
    // verifiable. Subsequent missed titles reuse only the album search cache.
    {
        Directory directory; unsigned songs = 0, albums = 0;
        auto dependencies = hooks();
        dependencies.http = [&](std::string_view url, std::size_t limit, HANDLE) {
            require(limit == 512 * 1024, "Bound song and album catalog responses");
            if (song_request(url)) {
                ++songs;
                Json original{{"kind", "song"}, {"trackName", "Song"}, {"artistName", edited.artist},
                    {"collectionName", edited.album}, {"artworkUrl100", thumbnail}, {"trackViewUrl", cover_page + "?i=456"}};
                return results(Json::array({original}));
            }
            require(album_request(url), "Exact album success should not fetch public pages");
            require(url.find("My%20Edit") == url.npos, "Album query sends only artist and album metadata");
            ++albums; return results(Json::array({album_row()}));
        };
        amp::ArtworkResolver resolver(settings(), directory.path, {}, dependencies);
        auto cover = resolver.resolve(edited);
        require(cover && !cover->animated && cover->track_url == cover_page
                && cover->url.ends_with("/1024x1024bb.jpg") && songs == 1 && albums == 1,
                "Edited song title uses verified exact album art");
        auto another = edited; another.title = "Song (Another Edit)";
        cover = resolver.resolve(another);
        require(cover && cover->track_url == cover_page && songs == 2 && albums == 1,
                "Search each song independently while reusing an exact album lookup");
        resolver.resolve(edited);
        require(songs == 2 && albums == 1, "Reuse exact track cache after album fallback");
        require(std::filesystem::is_empty(directory.path), "Normal album lookup writes no local cache");
    }
    {
        Directory directory; unsigned requests = 0;
        auto dependencies = hooks();
        dependencies.http = [&](std::string_view url, std::size_t, HANDLE) {
            require(song_request(url), "Verified song success never starts an album search"); ++requests;
            Json song{{"kind", "song"}, {"trackName", edited.title}, {"artistName", edited.artist},
                {"collectionName", edited.album}, {"artworkUrl100", thumbnail}, {"trackViewUrl", cover_page + "?i=456"}};
            return results(Json::array({song}));
        };
        amp::ArtworkResolver resolver(settings(), directory.path, {}, dependencies);
        require(resolver.resolve(edited).has_value() && requests == 1, "Keep song lookup as the preferred source");
    }
    // An album-only fallback cannot relax an edition or artist to obtain art.
    for (bool wrong_artist : {false, true}) {
        Directory directory; unsigned songs = 0, albums = 0, pages = 0;
        auto dependencies = hooks();
        dependencies.http = [&](std::string_view url, std::size_t, HANDLE) {
            if (song_request(url)) { ++songs; return results(Json::array()); }
            if (album_request(url)) {
                ++albums; return results(Json::array({album_row(wrong_artist ? "Different Artist" : edited.artist,
                    wrong_artist ? edited.album : edited.album + " (Deluxe)")}));
            }
            require(page_search(url), "Wrong album identity must not trigger image downloads");
            ++pages; return amp::HttpResponse{200, html(Json::array()), ""};
        };
        amp::ArtworkResolver resolver(settings(), directory.path, {}, dependencies);
        require(!resolver.resolve(edited) && songs == 1 && albums == 1 && pages == 1,
                "Reject wrong artist or edition before public page fallback");
    }
    // A certain static image does not imply a certain release or authorize a
    // motion publication when multiple exact albums share the same cover.
    {
        Directory directory; unsigned requests = 0; std::atomic<unsigned> motion_jobs{};
        auto dependencies = hooks();
        dependencies.http = [&](std::string_view url, std::size_t, HANDLE) {
            ++requests;
            if (song_request(url)) return results(Json::array());
            require(album_request(url), "Identical album covers require no page or image request");
            return results(Json::array({album_row(), album_row(edited.artist, edited.album,
                "https://music.apple.com/us/album/example/999")}));
        };
        dependencies.motion = [&](const amp::Track&, std::string_view, HANDLE) -> std::optional<amp::Artwork> {
            ++motion_jobs; return {};
        };
        {
            amp::ArtworkResolver resolver(settings(true), directory.path, {}, dependencies);
            auto cover = resolver.resolve(edited);
            require(cover && !cover->animated && cover->track_url.empty() && requests == 2,
                    "Shared album image has no arbitrary release link");
            amp::Snapshot snapshot; snapshot.track = edited; snapshot.state = amp::PlaybackState::playing;
            snapshot.observed_at = amp::unix_time();
            require(!amp::presence(snapshot, snapshot.observed_at, cover).contains("buttons"),
                    "Ambiguous album identity supplies no Listen button");
            for (unsigned i = 0; i < 10; ++i) require(!resolver.refresh(edited), "Shared static cover cannot start motion discovery");
        }
        require(motion_jobs == 0 && std::filesystem::is_empty(directory.path), "Never publish or persist motion for an ambiguous album");
    }
    {
        Directory directory; unsigned comparisons = 0, pages = 0;
        auto dependencies = hooks();
        dependencies.http = [&](std::string_view url, std::size_t, HANDLE) {
            if (song_request(url)) return results(Json::array());
            if (album_request(url)) return results(Json::array({album_row(), album_row(edited.artist, edited.album,
                "https://music.apple.com/us/album/example/999", "https://is1-ssl.mzstatic.com/image/thumb/different/100x100bb.jpg")}));
            if (url.starts_with("https://is1-ssl.mzstatic.com/"))
                return amp::HttpResponse{200, different_jpeg(++comparisons == 1 ? 'a' : 'b'), ""};
            ++pages;
            if (page_search(url)) return amp::HttpResponse{200, html(Json::array({public_song(edited)})), ""};
            require(url == fallback_page, "Only follow verified public album candidates");
            return amp::HttpResponse{200, public_album(edited), ""};
        };
        amp::ArtworkResolver resolver(settings(), directory.path, {}, dependencies);
        const auto cover = resolver.resolve(edited);
        require(cover && cover->track_url == fallback_page && comparisons == 2 && pages == 2,
                "Uncertain album images fall through to independently verified public track page");
    }
    // Cancelling between song and album requests interrupts the real default
    // 3.2-second rate-limit wait without dispatching a second HTTP operation.
    {
        Directory directory; Event fetched; std::atomic<unsigned> requests{};
        auto dependencies = hooks(); dependencies.request_interval = 3200ms;
        dependencies.http = [&](std::string_view url, std::size_t, HANDLE) {
            ++requests; require(song_request(url), "Cancelled album wait must not dispatch another request");
            SetEvent(fetched.handle); return results(Json::array());
        };
        amp::ArtworkResolver resolver(settings(), directory.path, {}, dependencies);
        std::exception_ptr failure;
        std::thread reader([&] { try { resolver.resolve(edited); } catch (...) { failure = std::current_exception(); } });
        const auto ready = WaitForSingleObject(fetched.handle, 2000);
        std::this_thread::sleep_for(30ms);
        const auto began = std::chrono::steady_clock::now(); resolver.cancel(); reader.join();
        require(ready == WAIT_OBJECT_0 && !failure && requests == 1
                && std::chrono::steady_clock::now() - began < 500ms,
                "Cancel native album rate-limit gap promptly with no second request");
    }
    // Cached album transport failures remain independent of per-track page
    // fallbacks: a new title can recover via its own verified public page.
    {
        Directory directory; unsigned songs = 0, albums = 0, searches = 0;
        auto recovered = edited; recovered.title = "Recovered Song";
        auto dependencies = hooks();
        dependencies.http = [&](std::string_view url, std::size_t, HANDLE) {
            if (song_request(url)) { ++songs; return results(Json::array()); }
            if (album_request(url)) { ++albums; return amp::HttpResponse{503, "", ""}; }
            if (page_search(url)) {
                ++searches;
                return amp::HttpResponse{200, html(searches == 1 ? Json::array() : Json::array({public_song(recovered)})), ""};
            }
            require(url == fallback_page, "Recover only from verified public album page");
            return amp::HttpResponse{200, public_album(recovered), ""};
        };
        amp::ArtworkResolver resolver(settings(), directory.path, {}, dependencies);
        require(!resolver.resolve(edited), "Initial temporary album failure leaves presence without guessed art");
        auto cover = resolver.resolve(recovered);
        require(cover && cover->track_url == fallback_page && songs == 2 && albums == 1 && searches == 2,
                "Cached temporary album failure does not hide independent page recovery for another song");
    }
}

#include "amp/artwork.hpp"
#include "amp/settings.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
struct MapFixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("amp-native-artwork-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    MapFixture() {
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Could not create isolated artwork fixture");
    }
    ~MapFixture() {
        std::error_code ignored;
        std::filesystem::remove(directory / "album_artwork.json", ignored);
        std::filesystem::remove(directory, ignored);
    }
};
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
nlohmann::json song(const std::string& image, const std::string& page,
                    const std::string& album = "After Hours (Deluxe)") {
    return {{"kind", "song"}, {"trackName", "Nothing Compares"}, {"artistName", "The Weeknd"},
            {"collectionName", album}, {"artworkUrl100", image}, {"trackViewUrl", page}};
}
}

void test_artwork() {
    using namespace amp;
    using namespace amp::artwork_detail;
    using Json = nlohmann::json;
    const Track track{"Nothing Compares", "The Weeknd", "After Hours (Deluxe)"};
    const std::string image = "https://is1-ssl.mzstatic.com/image/thumb/Music/example.jpg/100x100bb-60.jpg";
    const std::string sharp = "https://is1-ssl.mzstatic.com/image/thumb/Music/example.jpg/1024x1024bb.jpg";
    const std::string page = "https://music.apple.com/us/album/after-hours-deluxe/1615103111?i=1615103456";
    check(normalize("  AFTER Hours (Deluxe) ") == "after hours deluxe", "Normalize complete metadata");
    check(normalize("After Hours") != normalize("After Hours (Deluxe)"), "Preserve edition words");
    check(normalize("\xef\xbc\xa1\xef\xbc\xa2\xef\xbc\xa3") == "abc", "Normalize compatibility letters");
    check(normalize("Stra\xc3\x9f" "e") == "strasse", "Normalize sharp s");
    check(normalize(std::string("bad\xff", 4)).empty(), "Reject invalid UTF-8 metadata");
    check(normalize("Track \xf0\xa0\x80\x80") != normalize("Track \xf0\xa0\x80\x81"),
          "Preserve distinct supplementary Unicode characters");
    check(thumbnail(image) == sharp, "Upgrade recognized square Apple thumbnail");
    check(!thumbnail("https://evil.mzstatic.com.attacker.test/image/thumb/a/100x100bb.jpg"), "Reject misleading CDN hostname");
    check(!thumbnail("https://user@is1-ssl.mzstatic.com/a.jpg"), "Reject credentials in artwork URL");
    check(!thumbnail("http://is1-ssl.mzstatic.com/a.jpg"), "Require HTTPS artwork");
    check(!thumbnail("https://is1-ssl.mzstatic.com:444/a.jpg"), "Reject unexpected artwork port");
    check(thumbnail(image + "?token=example") == image + "?token=example", "Preserve signed thumbnail URL");
    check(thumbnail("https://is1-ssl.mzstatic.com/image/thumb/a/2048x2048bb.jpg")
           == "https://is1-ssl.mzstatic.com/image/thumb/a/2048x2048bb.jpg", "Preserve larger thumbnails");
    check(album_key(page) == "album:us:1615103111", "Use verified Apple album ID");
    check(!album_key("https://music.apple.com/us/album/after-hours/xyz"), "Reject malformed album ID");
    check(!album_key("https://music.apple.com.attacker.test/us/album/name/123"), "Reject false Apple host");

    Json payload{{"results", Json::array({song(image, page)})}};
    auto result = catalog_match(payload.dump(), track);
    check(result && result->url == sharp && result->track_url == page && !result->animated,
          "Resolve exact title artist and edition");
    payload["results"][0]["collectionName"] = "After Hours";
    check(!catalog_match(payload.dump(), track), "Never replace deluxe edition with standard album");
    payload["results"] = Json::array({song(image, page), song(image, "https://music.apple.com/us/album/other/999?i=1")});
    result = catalog_match(payload.dump(), track);
    check(result && result->url == sharp && result->track_url.empty(), "Accept identical covers without guessing release link");
    payload["results"][1]["artworkUrl100"] = "https://is1-ssl.mzstatic.com/image/thumb/other/100x100bb.jpg";
    check(!catalog_match(payload.dump(), track), "Reject multiple distinct covers");
    payload["results"] = Json::array({song(image, "https://evil.test/album/123")});
    check(!catalog_match(payload.dump(), track), "Require verified store button URL");
    payload["results"][0]["trackName"] = 123;
    check(!catalog_match(payload.dump(), track), "Ignore incorrect catalog field types");
    bool rejected = false;
    try { catalog_match(std::string(100, '[') + "0" + std::string(100, ']'), track); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, "Bound JSON nesting");
    rejected = false;
    try { catalog_match(std::string(512 * 1024 + 1, ' '), track); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, "Bound JSON size");

    Json mapping{{"version", 1}, {"albums", Json::array({{{"artist", "The Weeknd"},
        {"album", "After Hours (Deluxe)"}, {"image_url", "https://example.com/cover.webp"},
        {"album_url", "https://music.apple.com/us/album/name/1615103111"}}})}};
    result = mapped_cover(mapping.dump(), track);
    check(result && result->animated, "Explicit animated mapping matches edition");
    mapping["albums"].push_back(mapping["albums"][0]);
    mapping["albums"][1]["image_url"] = "https://example.com/other.webp";
    check(!mapped_cover(mapping.dump(), track), "Reject conflicting explicit mappings");
    mapping["albums"] = Json::array({mapping["albums"][0]});
    mapping["albums"][0]["image_url"] = "https://example.com/cover.webp?token=secret";
    check(!mapped_cover(mapping.dump(), track), "Reject tokens in mapped animation URL");

    const std::string repository = "jacobortiz/apple-music-presence";
    const std::string motion = "https://raw.githubusercontent.com/" + repository + "/"
        + std::string(40, 'a') + "/artwork/motion/1615103111-abcdef123456.webp";
    const std::string key = "album:us:1615103111";
    Json cache{{"repository", repository}, {"encoding_profile", "webp-768-q85-lanczos-v2"},
               {"albums", {{key, {{"expires", 1100}, {"cover", {{"url", motion}, {"track_url", page}}}}}}}};
    result = cached_motion(cache.dump(), repository, page, 1000);
    check(result && result->url == motion && result->animated && result->track_url.find('?') == std::string::npos,
          "Reuse valid immutable motion cover for verified album");
    check(!cached_motion(cache.dump(), repository, "https://music.apple.com/us/album/name/999", 1000),
          "Never reuse motion cover for another release");
    check(!cached_motion(cache.dump(), "different/repo", page, 1000), "Require matching public artwork host");
    check(!cached_motion(cache.dump(), repository, page, 1200), "Ignore expired motion cache");
    cache["encoding_profile"] = "old-profile";
    check(!cached_motion(cache.dump(), repository, page, 1000), "Ignore outdated conversion profile");
    cache["encoding_profile"] = "webp-768-q85-lanczos-v2";
    cache["albums"][key]["cover"]["url"] = "https://raw.githubusercontent.com/" + repository + "/"
        + std::string(40, 'a') + "/artwork/motion/999-abcdef123456.webp";
    check(!cached_motion(cache.dump(), repository, page, 1000), "Require cached filename to match verified album ID");
    cache["albums"][key]["cover"]["url"] = "https://raw.githubusercontent.com/" + repository + "/main/artwork/cover.webp";
    check(!cached_motion(cache.dump(), repository, page, 1000), "Require immutable commit motion URL");

    Settings disabled;
    disabled.artwork = false;
    ArtworkResolver resolver(disabled, std::filesystem::temp_directory_path());
    check(!resolver.resolve(track), "No artwork lookup when sharing metadata is disabled");
    Settings mapped;
    mapped.artwork = true;
    mapped.motion_artwork = true;
    ArtworkResolver mapped_resolver(mapped, std::filesystem::temp_directory_path() / "amp-native-artwork-offline-tests");
    result = mapped_resolver.resolve({"Blinding Lights", "The Weeknd", "After Hours"});
    check(result && result->animated && result->url.ends_with("after-hours-hq.webp"), "Bundled explicit map works offline");
    check(!mapped_resolver.resolve({"", "The Weeknd", "After Hours"}), "Skip incomplete metadata without network");
    MapFixture fixture;
    Json custom{{"version", 1}, {"albums", Json::array({{{"artist", "Example Artist"},
        {"album", "Example Album"}, {"image_url", "https://example.com/custom.webp"},
        {"album_url", "https://music.apple.com/us/album/example/123"}}})}};
    {
        std::ofstream file(fixture.directory / "album_artwork.json", std::ios::binary);
        file << custom.dump(); file.close();
        check(static_cast<bool>(file), "Write isolated custom artwork fixture");
    }
    ArtworkResolver custom_resolver(mapped, fixture.directory);
    result = custom_resolver.resolve({"Example Track", "Example Artist", "Example Album"});
    check(result && result->animated && result->url == "https://example.com/custom.webp",
          "Reuse existing custom artwork filename without catalog lookup");
}

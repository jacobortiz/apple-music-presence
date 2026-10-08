#include "amp/motion.hpp"

#include <cassert>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
constexpr auto page = "https://music.apple.com/us/album/album/123";
constexpr auto stream = "https://mvod.itunes.apple.com/itunes-assets/motion/master.m3u8";

void rejects(const std::function<void()>& action) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}

nlohmann::json header(const std::string& id = "123", const std::string& album = "Album") {
    return {{"title", album}, {"contentDescriptor", {{"kind", "album"}, {"identifiers", {{"storeAdamID", id}}}}},
            {"videoArtwork", {{"dictionary", {{"motionDetailSquare", {{"video", stream}}}}}}}};
}

std::string html(const nlohmann::json& value) { return "<script type=\"application/json\">" + value.dump() + "</script>"; }

void append_u32(std::string& output, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) output += static_cast<char>(value >> (byte * 8));
}

std::string chunk(std::string_view kind, std::string_view value) {
    std::string result(kind);
    append_u32(result, static_cast<std::uint32_t>(value.size()));
    result += value;
    if (value.size() % 2) result += '\0';
    return result;
}

std::string webp(std::string contents) {
    std::string result = "RIFF";
    append_u32(result, static_cast<std::uint32_t>(contents.size() + 4));
    return result + "WEBP" + contents;
}
}

void test_motion() {
    using namespace amp::motion_detail;
    assert(apple_stream(stream));
    for (const auto* invalid : {"http://mvod.itunes.apple.com/x.m3u8", "https://evil.test/x.m3u8",
                               "https://user:secret@mvod.itunes.apple.com/x.m3u8",
                               "https://mvod.itunes.apple.com:444/x.m3u8", "https://mvod.itunes.apple.com/x.m3u8?token=fixture",
                               "https://mvod.itunes.apple.com/x.m3u8#fragment", "https://mvod.itunes.apple.com/x.mp4"})
        assert(!apple_stream(invalid));
    assert(album_page(std::string(page) + "?i=456&token=fixture") == page);
    assert(!album_page("https://music.apple.com/us/song/123"));
    assert(album_redirect(page, "/us/album/new-slug/123?i=456") == "https://music.apple.com/us/album/new-slug/123");
    for (const auto* invalid : {"/us/album/new-slug/999", "/gb/album/new-slug/123",
                               "https://evil.test/us/album/new-slug/123", "https://user:secret@music.apple.com/us/album/new-slug/123",
                               "http://music.apple.com/us/album/new-slug/123", "https://music.apple.com:444/us/album/new-slug/123"})
        assert(!album_redirect(page, invalid));

    assert(find_motion(html(header()), "123", "Album") == stream);
    auto no_motion = header();
    no_motion["videoArtwork"] = nullptr;
    assert(!find_motion(html(no_motion), "123", "Album"));
    auto untrusted = header();
    untrusted["videoArtwork"]["dictionary"]["motionDetailSquare"]["video"] = "https://evil.test/x.m3u8";
    assert(!find_motion(html(untrusted), "123", "Album"));
    rejects([&] { find_motion(html(header("999")), "123", "Album"); });
    rejects([&] { find_motion(html(header("123", "Album (Deluxe)")), "123", "Album"); });
    rejects([&] { find_motion("<html>Unknown format</html>", "123", "Album"); });
    auto alternate = header();
    alternate["videoArtwork"]["dictionary"]["motionDetailSquare"]["video"] = "https://mvod.itunes.apple.com/other.m3u8";
    rejects([&] { find_motion(html(nlohmann::json::array({header(), alternate})), "123", "Album"); });
    rejects([&] { find_motion("<script>" + std::string(100, '[') + "0" + std::string(100, ']') + "</script>", "123", "Album"); });

    const std::string master =
        "#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"avc1.64001f\",RESOLUTION=408x408\n408.m3u8\n"
        "#EXT-X-STREAM-INF:CODECS=\"avc1.640028\",RESOLUTION=720x720\n720.m3u8\n"
        "#EXT-X-STREAM-INF:CODECS=\"avc1.64002a\",RESOLUTION=1080x1080,VIDEO-RANGE=SDR\n1080.m3u8\n"
        "#EXT-X-STREAM-INF:CODECS=\"avc1.64002a\",RESOLUTION=1920x1920\n1920.m3u8\n";
    auto variants = rendition_urls(master, stream);
    assert(variants.size() == 2 && variants[0].ends_with("/1080.m3u8") && variants[1].ends_with("/720.m3u8"));
    rejects([&] { rendition_urls("#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"hvc1.1.6\",RESOLUTION=1080x1080\nx.m3u8\n", stream); });
    rejects([&] { rendition_urls("#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"avc1.64001f\",RESOLUTION=1080x1080,VIDEO-RANGE=PQ\nx.m3u8\n", stream); });
    rejects([&] { rendition_urls("#EXTM3U\n#EXT-X-STREAM-INF:CODECS=\"avc1.64001f\",RESOLUTION=408x408\nhttps://evil.test/x.m3u8\n", stream); });

    const std::string variant =
        "#EXTM3U\n#EXT-X-MAP:URI=\"clip.mp4\",BYTERANGE=\"20@0\"\n"
        "#EXTINF:4.0,\n#EXT-X-BYTERANGE:20@20\nclip.mp4\n"
        "#EXTINF:4.0,\n#EXT-X-BYTERANGE:20@40\nclip.mp4\n#EXT-X-ENDLIST\n";
    const auto local = rewrite_playlist(variant, stream);
    assert(local.media.size() == 1 && local.media[0].second == "segment-0.mp4");
    assert(local.media[0].first == "https://mvod.itunes.apple.com/itunes-assets/motion/clip.mp4");
    assert(local.local.find("https:") == std::string::npos && local.local.find("clip.mp4") == std::string::npos);
    assert(local.local.find("#EXT-X-BYTERANGE:20@40") != std::string::npos);
    assert(local.local.find("#EXT-X-MAP:URI=\"segment-0.mp4\",BYTERANGE=\"20@0\"") != std::string::npos);
    // Reconstruct MAP instead of replacing the first matching raw substring.
    const auto tricky = rewrite_playlist("#EXTM3U\n#EXT-X-MAP:BYTERANGE=\"20@0\",URI=\"20\"\n#EXTINF:4,\n20\n#EXT-X-ENDLIST\n", stream);
    assert(tricky.local.find("URI=\"segment-0.mp4\"") != std::string::npos);
    for (const auto* invalid : {
            "#EXTM3U\n#EXTINF:4,\nhttps://evil.test/clip.mp4\n#EXT-X-ENDLIST\n",
            "#EXTM3U\n#EXTINF:4,\nclip.mp4?token=fixture\n#EXT-X-ENDLIST\n",
            "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXTINF:4,\nclip.mp4\n#EXT-X-ENDLIST\n",
            "#EXTM3U\n#EXTINF:61,\nclip.mp4\n#EXT-X-ENDLIST\n",
            "#EXTM3U\n#EXTINF:4,\nclip.mp4\n",
            "#EXTM3U\n#EXTINF:nan,\nclip.mp4\n#EXT-X-ENDLIST\n",
            "#EXTM3U\n#EXT-X-MEDIA:URI = \"C:/private.mp4\"\n#EXTINF:4,\nclip.mp4\n#EXT-X-ENDLIST\n",
            "#EXTM3U\n#EXT-X-MAP:URI=\"clip.mp4\",URI=\"file:///private.mp4\"\n#EXTINF:4,\nclip.mp4\n#EXT-X-ENDLIST\n"})
        rejects([&] { rewrite_playlist(invalid, stream); });
    std::string excessive = "#EXTM3U\n";
    for (unsigned index = 0; index < 33; ++index) excessive += "#EXTINF:1,\nclip-" + std::to_string(index) + ".mp4\n";
    excessive += "#EXT-X-ENDLIST\n";
    rejects([&] { rewrite_playlist(excessive, stream); });

    const auto extended = chunk("VP8X", std::string(10, '\0'));
    const auto animation = chunk("ANIM", std::string(6, '\0'));
    const auto frame = chunk("ANMF", std::string(16, '\0'));
    const auto valid = webp(extended + animation + frame + frame);
    assert(animated_webp(valid));
    assert(!animated_webp(webp(extended + animation + frame)));
    assert(!animated_webp(webp(extended + animation + frame + frame + chunk("EXIF", "private fixture"))));
    assert(!animated_webp(webp(extended + animation + frame + frame + chunk("XMP ", "private fixture"))));
    assert(!animated_webp(valid + "trailing bytes"));
    assert(!animated_webp(std::string(amp::max_motion_image_bytes + 1, 'x')));

    // An already-cancelled job must never reach HTTP, create files, or start an encoder.
    HANDLE stopped = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    assert(stopped);
    rejects([&] { amp::discover_motion(page, "Album", stopped); });
    rejects([&] { amp::convert_motion(stream, {}, {}, stopped); });
    rejects([&] { amp::prepare_motion(page, "Album", {}, {}, stopped); });
    CloseHandle(stopped);
}

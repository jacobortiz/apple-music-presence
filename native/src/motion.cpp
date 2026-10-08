#include "amp/motion.hpp"
#include "amp/artwork.hpp"
#include "amp/http.hpp"
#include "amp/process.hpp"
#include "amp/settings.hpp"
#include <Objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <stdexcept>

namespace amp {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_playlist = 128 * 1024;
constexpr std::size_t max_page = 3 * 1024 * 1024;

void checkpoint(HANDLE stop) {
    if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
        throw std::runtime_error("Motion preparation cancelled");
}

std::string lower_ascii(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
    return value;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

struct Url { std::string host, path; };
std::optional<Url> url_parts(std::string_view value, bool permit_query = false) {
    if (value.size() > 2048 || value.size() < 9 ||
        lower_ascii(std::string(value.substr(0, 8))) != "https://" ||
        value.find('\\') != std::string_view::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return {};
    const auto end = value.find_first_of("/?#", 8);
    auto host = lower_ascii(std::string(value.substr(8, end == std::string_view::npos ? value.size() - 8 : end - 8)));
    if (host.ends_with(":443")) host.resize(host.size() - 4);
    if (host.empty() || host.find_first_of(":@") != std::string::npos ||
        !std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
        })) return {};
    const auto query = value.find_first_of("?#", end);
    if (!permit_query && query != std::string_view::npos) return {};
    auto path = end == std::string_view::npos ? std::string("/") :
        std::string(value.substr(end, query == std::string_view::npos ? value.size() - end : query - end));
    if (path.empty() || path.front() != '/') return {};
    return Url{std::move(host), std::move(path)};
}

std::optional<std::string> join_url(std::string_view base, std::string_view reference) {
    if (reference.empty() || reference.size() > 2048 || reference.find_first_of("\\?#") != std::string_view::npos ||
        std::any_of(reference.begin(), reference.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return {};
    if (reference.find("://") != std::string_view::npos) {
        return url_parts(reference) ? std::optional<std::string>(reference) : std::nullopt;
    }
    if (reference.starts_with("//")) {
        const auto absolute = "https:" + std::string(reference);
        return url_parts(absolute) ? std::optional<std::string>(absolute) : std::nullopt;
    }
    if (reference.find(':') != std::string_view::npos) return {};
    const auto source = url_parts(base);
    if (!source) return {};
    std::string path = reference.starts_with('/') ? std::string(reference) :
        source->path.substr(0, source->path.rfind('/') + 1) + std::string(reference);
    std::vector<std::string> parts;
    for (std::size_t start = 1; start <= path.size();) {
        const auto slash = path.find('/', start);
        auto part = path.substr(start, slash == std::string::npos ? path.size() - start : slash - start);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (part != "." && !part.empty()) parts.push_back(std::move(part));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string joined = "https://" + source->host;
    for (const auto& part : parts) joined += "/" + part;
    return url_parts(joined) ? std::optional<std::string>(joined) : std::nullopt;
}

std::vector<std::string> lines(std::string_view value) {
    std::vector<std::string> result;
    for (std::size_t start = 0; start < value.size();) {
        const auto end = value.find('\n', start);
        auto line = trim(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (!line.empty()) result.push_back(std::move(line));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::map<std::string, std::string> attributes(std::string_view input) {
    std::map<std::string, std::string> result;
    while (!input.empty()) {
        const auto equal = input.find('=');
        if (equal == std::string_view::npos || equal == 0) throw std::runtime_error("Invalid HLS attributes");
        const auto key = trim(input.substr(0, equal));
        input.remove_prefix(equal + 1);
        std::string value;
        if (input.starts_with('"')) {
            const auto end = input.find('"', 1);
            if (end == std::string_view::npos) throw std::runtime_error("Invalid HLS quoted attribute");
            value = input.substr(1, end - 1);
            input.remove_prefix(end + 1);
            if (!input.empty() && input.front() != ',') throw std::runtime_error("Invalid HLS attribute boundary");
        } else {
            const auto end = input.find(',');
            value = trim(input.substr(0, end));
            input.remove_prefix(end == std::string_view::npos ? input.size() : end);
        }
        if (!result.emplace(key, value).second) throw std::runtime_error("Duplicate HLS attribute");
        if (!input.empty()) input.remove_prefix(1);
    }
    return result;
}

std::string field(const Json& object, const char* name) {
    if (!object.is_object()) return {};
    const auto item = object.find(name);
    return item != object.end() && item->is_string() ? item->get<std::string>() : "";
}

const Json* child(const Json& object, const char* name) {
    if (!object.is_object()) return nullptr;
    const auto item = object.find(name);
    return item == object.end() ? nullptr : &*item;
}

std::string identifier(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return std::to_string(value.get<std::int64_t>());
    return {};
}

std::string fetch(std::string_view url, std::size_t limit, HANDLE stop) {
    checkpoint(stop);
    const auto response = http_get(url, limit, stop);
    if (response.status != 200) throw std::runtime_error("Apple motion request failed");
    return response.body;
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("Could not write temporary motion data");
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::filesystem::path& parent) {
        std::filesystem::create_directories(parent);
        parent_ = std::filesystem::weakly_canonical(std::filesystem::absolute(parent));
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a motion workspace");
        wchar_t text[40]{};
        if (!StringFromGUID2(id, text, 40)) throw std::runtime_error("Could not name a motion workspace");
        path_ = parent_ / (L"motion-" + std::wstring(text));
        if (path_.parent_path() != parent_ || !std::filesystem::create_directory(path_))
            throw std::runtime_error("Could not create a motion workspace");
    }
    ~TemporaryDirectory() {
        // A generated child is the only directory owned by this job. Re-check
        // the resolved path before recursively removing any temporary files.
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path_, error);
        if (!error && resolved.parent_path() == parent_ && resolved == path_)
            std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path parent_, path_;
};

std::filesystem::path download_rendition(std::string_view variant,
                                       const std::filesystem::path& directory, HANDLE stop) {
    auto playlist = motion_detail::rewrite_playlist(fetch(variant, max_playlist, stop), variant);
    std::size_t total{};
    for (const auto& [url, name] : playlist.media) {
        auto content = fetch(url, 8 * 1024 * 1024, stop);
        total += content.size();
        if (total > 24 * 1024 * 1024) throw HttpSizeError("Motion download exceeded size limit");
        checkpoint(stop);
        write_file(directory / name, content);
    }
    checkpoint(stop);
    const auto local = directory / L"motion.m3u8";
    write_file(local, playlist.local);
    return local;
}

std::filesystem::path download_video(std::string_view stream,
                                    const std::filesystem::path& directory, HANDLE stop) {
    if (!motion_detail::apple_stream(stream)) throw std::runtime_error("Invalid Apple motion stream");
    const auto variants = motion_detail::rendition_urls(fetch(stream, max_playlist, stop), stream);
    for (std::size_t index = 0; index < variants.size(); ++index) {
        try { return download_rendition(variants[index], directory, stop); }
        catch (const HttpSizeError&) { if (index + 1 == variants.size()) throw; }
    }
    throw std::runtime_error("No supported motion rendition");
}

std::uint32_t u32(std::string_view content, std::size_t offset) {
    std::uint32_t value{};
    for (unsigned byte = 0; byte < 4; ++byte)
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(content[offset + byte])) << (byte * 8);
    return value;
}

std::filesystem::path app_directory() {
    std::wstring value(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    if (!size || size == value.size()) throw std::runtime_error("Could not locate the app directory");
    value.resize(size);
    return std::filesystem::path(value).parent_path();
}
}

namespace motion_detail {
bool apple_stream(std::string_view value) {
    const auto url = url_parts(value);
    return url && url->host == "mvod.itunes.apple.com" && url->path.ends_with(".m3u8");
}

std::optional<std::string> album_page(std::string_view value) {
    const auto url = url_parts(value, true);
    if (!url || url->host != "music.apple.com") return {};
    static const std::regex path(R"(^/([A-Za-z]{2})/album/[^/?#]+/([0-9]{1,20})/?$)");
    if (!std::regex_match(url->path, path)) return {};
    return "https://music.apple.com" + url->path;
}

std::optional<std::string> album_redirect(std::string_view source, std::string_view location) {
    const auto from = album_page(source);
    if (!from || location.empty()) return {};
    // Remove Apple track queries before constructing the next public request.
    const auto end = location.find_first_of("?#");
    location = location.substr(0, end);
    const auto joined = join_url(*from, location);
    if (!joined) return {};
    const auto target = album_page(*joined);
    if (!target || artwork_detail::album_key(*target) != artwork_detail::album_key(*from)) return {};
    return target;
}

std::optional<std::string> find_motion(std::string_view html, std::string_view album_id,
                                      std::string_view album) {
    if (html.size() > max_page) throw std::runtime_error("Apple album page is too large");
    const auto expected_title = artwork_detail::normalize(album);
    if (expected_title.empty()) throw std::runtime_error("Motion discovery requires album metadata");
    bool found_header{};
    std::set<std::string> streams;
    auto walk = [&](auto&& self, const Json& value) -> void {
        if (value.is_object()) {
            const auto descriptor = child(value, "contentDescriptor");
            const auto identifiers = descriptor ? child(*descriptor, "identifiers") : nullptr;
            const auto id = identifiers ? child(*identifiers, "storeAdamID") : nullptr;
            const auto artwork = child(value, "videoArtwork");
            if (descriptor && field(*descriptor, "kind") == "album" && id &&
                identifier(*id) == album_id && artwork &&
                artwork_detail::normalize(field(value, "title")) == expected_title) {
                found_header = true;
                const auto dictionary = child(*artwork, "dictionary");
                const auto square = dictionary ? child(*dictionary, "motionDetailSquare") : nullptr;
                const auto stream = square ? field(*square, "video") : "";
                if (apple_stream(stream)) streams.insert(stream);
            }
            for (const auto& item : value) self(self, item);
        } else if (value.is_array()) for (const auto& item : value) self(self, item);
    };
    const auto lower = lower_ascii(std::string(html));
    for (std::size_t position = 0;;) {
        const auto start = lower.find("<script", position);
        if (start == std::string::npos) break;
        position = start + 7;
        if (position < lower.size() && lower[position] != '>' && lower[position] != ' ' &&
            lower[position] != '\t' && lower[position] != '\r' && lower[position] != '\n') continue;
        const auto open = lower.find('>', position);
        if (open == std::string::npos) break;
        const auto close = lower.find("</script", open + 1);
        if (close == std::string::npos) break;
        position = close + 8;
        try {
            const auto value = Json::parse(html.substr(open + 1, close - open - 1),
                [](int depth, auto, auto&) {
                    if (depth > 64) throw std::runtime_error("Motion JSON nesting limit");
                    return true;
                });
            walk(walk, value);
        } catch (const Json::exception&) { /* Ignore script code, not album data. */ }
          catch (const std::runtime_error&) { /* Over-deep optional script is invalid. */ }
    }
    if (!found_header) throw std::runtime_error("Apple album page format or metadata did not match");
    if (streams.size() > 1) throw std::runtime_error("Ambiguous motion cover");
    return streams.empty() ? std::nullopt : std::optional<std::string>(*streams.begin());
}

std::vector<std::string> rendition_urls(std::string_view master, std::string_view stream) {
    if (master.size() > max_playlist || !apple_stream(stream)) throw std::runtime_error("Invalid motion master playlist");
    const auto text = lines(master);
    if (text.empty() || text.front() != "#EXTM3U") throw std::runtime_error("Invalid motion master playlist");
    std::vector<std::pair<unsigned, std::string>> larger, smaller;
    static const std::regex resolution(R"(^([0-9]{1,5})x([0-9]{1,5})$)");
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (!text[index].starts_with("#EXT-X-STREAM-INF:")) continue;
        const auto values = attributes(std::string_view(text[index]).substr(18));
        const auto codec = values.find("CODECS"), size = values.find("RESOLUTION");
        if (codec == values.end() || !codec->second.starts_with("avc1.") || size == values.end() ||
            (values.contains("VIDEO-RANGE") && values.at("VIDEO-RANGE") != "SDR")) continue;
        std::smatch match;
        if (!std::regex_match(size->second, match, resolution) || match[1] != match[2]) continue;
        const auto pixels = static_cast<unsigned>(std::stoul(match[1]));
        if (pixels == 0 || pixels > 1080) continue;
        const auto url = join_url(stream, text[index + 1]);
        if (!url || !apple_stream(*url)) continue;
        (pixels >= 768 ? larger : smaller).emplace_back(pixels, *url);
    }
    if (larger.empty() && smaller.empty()) throw std::runtime_error("No supported square SDR rendition");
    std::sort(larger.begin(), larger.end());
    std::sort(smaller.begin(), smaller.end(), std::greater<>());
    std::vector<std::string> result{!larger.empty() ? larger.front().second : smaller.front().second};
    if (!larger.empty() && !smaller.empty()) result.push_back(smaller.front().second);
    return result;
}

Playlist rewrite_playlist(std::string_view playlist, std::string_view variant) {
    if (playlist.size() > max_playlist || !apple_stream(variant)) throw std::runtime_error("Invalid motion playlist");
    const auto text = lines(playlist);
    if (text.empty() || text.front() != "#EXTM3U" ||
        std::find(text.begin(), text.end(), "#EXT-X-ENDLIST") == text.end() ||
        std::any_of(text.begin(), text.end(), [](const auto& line) { return line.starts_with("#EXT-X-KEY"); }))
        throw std::runtime_error("Only finite, unencrypted motion covers are supported");
    Playlist result;
    std::map<std::string, std::string> names;
    double duration{};
    bool pending_segment{};
    for (const auto& line : text) {
        if (line.starts_with("#EXTINF:")) {
            if (pending_segment) throw std::runtime_error("Motion duration has no segment");
            const auto end = line.find(',');
            const auto number = line.substr(8, end == std::string::npos ? line.size() - 8 : end - 8);
            static const std::regex decimal(R"(^[0-9]+(?:\.[0-9]+)?$)");
            if (!std::regex_match(number, decimal)) throw std::runtime_error("Invalid motion duration");
            const auto seconds = std::stod(number);
            duration += seconds;
            if (!std::isfinite(seconds) || seconds <= 0 || !std::isfinite(duration) || duration > 60)
                throw std::runtime_error("Motion cover exceeds 60 seconds");
            pending_segment = true;
        }
        std::string raw, rewritten = line;
        std::optional<std::string> map_range;
        if (line.starts_with("#EXT-X-MAP:")) {
            const auto values = attributes(std::string_view(line).substr(11));
            if (!values.contains("URI") || std::any_of(values.begin(), values.end(), [](const auto& item) {
                return item.first != "URI" && item.first != "BYTERANGE";
            })) throw std::runtime_error("Invalid initialization segment");
            raw = values.at("URI");
            if (values.contains("BYTERANGE")) {
                static const std::regex range(R"(^[0-9]{1,20}(?:@[0-9]{1,20})?$)");
                if (!std::regex_match(values.at("BYTERANGE"), range))
                    throw std::runtime_error("Invalid initialization byte range");
                map_range = values.at("BYTERANGE");
            }
        } else if (!line.starts_with('#')) {
            if (!pending_segment) throw std::runtime_error("Motion segment has no duration");
            pending_segment = false;
            raw = line;
        } else {
            static const std::regex media_reference(R"(URI\s*=)", std::regex::icase);
            if (std::regex_search(line, media_reference))
                throw std::runtime_error("Unsupported HLS media reference");
        }
        if (!raw.empty()) {
            const auto url = join_url(variant, raw);
            const auto parsed = url ? url_parts(*url) : std::nullopt;
            if (!parsed || parsed->host != "mvod.itunes.apple.com")
                throw std::runtime_error("Motion segment must be on Apple's media host");
            const auto [entry, inserted] = names.emplace(*url, "segment-" + std::to_string(names.size()) + ".mp4");
            if (inserted) result.media.emplace_back(*url, entry->second);
            if (names.size() > 32) throw std::runtime_error("Too many motion segments");
            if (line.starts_with('#')) {
                rewritten = "#EXT-X-MAP:URI=\"" + entry->second + "\"";
                if (map_range) rewritten += ",BYTERANGE=\"" + *map_range + "\"";
            } else rewritten = entry->second;
        } else if (line.starts_with("#EXT-X-MAP:")) throw std::runtime_error("Invalid initialization segment");
        result.local += rewritten + '\n';
    }
    if (pending_segment || duration <= 0 || result.media.empty()) throw std::runtime_error("Invalid finite motion playlist");
    return result;
}

bool animated_webp(std::string_view content) {
    if (content.size() < 20 || content.size() > max_motion_image_bytes || content.substr(0, 4) != "RIFF" ||
        content.substr(8, 4) != "WEBP" || static_cast<std::size_t>(u32(content, 4)) + 8 != content.size()) return false;
    std::size_t offset = 12, frames{};
    bool animation{};
    while (offset + 8 <= content.size()) {
        const auto kind = content.substr(offset, 4);
        const auto size = static_cast<std::size_t>(u32(content, offset + 4));
        if ((kind != "VP8X" && kind != "ANIM" && kind != "ANMF") || size > content.size() - offset - 8) return false;
        animation = animation || (kind == "ANIM" && size == 6);
        frames += kind == "ANMF" && size >= 16;
        offset += 8 + size + size % 2;
    }
    return offset == content.size() && animation && frames > 1;
}
}

std::optional<std::string> discover_motion(std::string_view verified_page,
                                         std::string_view album, HANDLE stop) {
    auto page = motion_detail::album_page(verified_page);
    if (!page) throw std::runtime_error("Motion discovery requires a verified Apple album page");
    const auto key = artwork_detail::album_key(*page);
    const auto id = key->substr(key->rfind(':') + 1);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        checkpoint(stop);
        const auto response = http_get(*page, max_page, stop);
        if (response.status == 200) return motion_detail::find_motion(response.body, id, album);
        if (attempt == 2 || (response.status != 301 && response.status != 302 && response.status != 307 && response.status != 308))
            throw std::runtime_error("Apple album motion page request failed");
        const auto redirected = motion_detail::album_redirect(*page, response.location);
        if (!redirected) throw std::runtime_error("Apple album redirect changed its verified identity");
        page = redirected;
    }
    throw std::runtime_error("Apple motion redirect limit");
}

std::filesystem::path ffmpeg_executable(const std::filesystem::path& app) {
    const auto sibling = std::filesystem::absolute(app / L"ffmpeg.exe");
    std::error_code error;
    if (std::filesystem::is_regular_file(sibling, error)) return sibling;
    const auto required = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (required && required <= 32768) {
        std::wstring path(required, L'\0');
        const auto written = GetEnvironmentVariableW(L"PATH", path.data(), required);
        if (written && written < required) {
            path.resize(written);
            for (std::size_t start = 0; start < path.size();) {
                const auto end = path.find(';', start);
                auto part = path.substr(start, end == std::wstring::npos ? path.size() - start : end - start);
                if (part.size() >= 2 && part.front() == '"' && part.back() == '"') part = part.substr(1, part.size() - 2);
                const std::filesystem::path directory(part);
                if (directory.is_absolute() && std::filesystem::is_regular_file(directory / L"ffmpeg.exe", error))
                    return directory / L"ffmpeg.exe";
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
        }
    }
    throw std::runtime_error("New animated covers need ffmpeg.exe beside the app or on PATH");
}

std::string convert_motion(std::string_view stream, const std::filesystem::path& ffmpeg,
                           const std::filesystem::path& temp_parent, HANDLE stop) {
    checkpoint(stop);
    if (!motion_detail::apple_stream(stream)) throw std::runtime_error("Invalid Apple motion stream");
    if (ffmpeg.empty() || !std::filesystem::is_regular_file(ffmpeg)) throw std::runtime_error("FFmpeg is unavailable");
    TemporaryDirectory temporary(temp_parent);
    const auto local = download_video(stream, temporary.path(), stop);
    const auto output = temporary.path() / L"cover.webp";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (const auto [size, fps] : std::array<std::pair<unsigned, unsigned>, 3>{{{768, 15}, {768, 10}, {512, 10}}}) {
        checkpoint(stop);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) throw std::runtime_error("Motion conversion timed out");
        const auto filter = L"fps=" + std::to_wstring(fps) + L",scale=" + std::to_wstring(size) + L":" +
            std::to_wstring(size) + L":flags=lanczos,setpts=PTS-STARTPTS";
        const auto result = run_process(ffmpeg,
            {L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-y", L"-protocol_whitelist", L"file",
             L"-allowed_extensions", L"ALL", L"-i", local.wstring(), L"-map", L"0:v:0", L"-vf", filter,
             L"-t", L"60", L"-map_metadata", L"-1", L"-map_metadata:s:v", L"-1", L"-map_chapters", L"-1",
             L"-an", L"-sn", L"-dn", L"-c:v", L"libwebp_anim", L"-loop", L"0", L"-quality", L"85", output.wstring()},
            {}, remaining, stop);
        checkpoint(stop);
        if (result.exit_code != 0) throw std::runtime_error("Motion conversion failed");
        std::ifstream input(output, std::ios::binary);
        if (!input) throw std::runtime_error("Motion conversion did not create an image");
        std::string content(max_motion_image_bytes + 1, '\0');
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad()) throw std::runtime_error("Could not read converted motion cover");
        if (content.size() > max_motion_image_bytes) continue;
        if (!motion_detail::animated_webp(content)) throw std::runtime_error("Converted cover is not animated WebP");
        return content;
    }
    throw std::runtime_error("Converted cover exceeds size limit");
}

std::optional<PreparedMotion> prepare_motion(std::string_view verified_page, std::string_view album,
                                            const std::filesystem::path& ffmpeg,
                                            const std::filesystem::path& temp_parent, HANDLE stop) {
    const auto stream = discover_motion(verified_page, album, stop);
    if (!stream) return {};
    const auto encoder = ffmpeg.empty() ? ffmpeg_executable(app_directory()) : ffmpeg;
    return PreparedMotion{*stream, convert_motion(*stream, encoder, temp_parent, stop)};
}
}

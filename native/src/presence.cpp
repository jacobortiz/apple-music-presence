#include <amp/presence.hpp>
#include <amp/settings.hpp>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace amp {
static std::string discord_text(const std::string& value) {
    auto input = wide(value);
    std::wstring result;
    bool space{};
    for (wchar_t c : input) {
        if (iswspace(c)) { space = !result.empty(); continue; }
        if (space && result.size() < 128) result += L' ';
        space = false;
        if (result.size() >= 128) break;
        result += c;
    }
    // Truncation must not split a UTF-16 surrogate pair.
    if (!result.empty() && result.back() >= 0xD800 && result.back() <= 0xDBFF) result.pop_back();
    if (result.size() < 2) result += L'\x200b';
    return utf8(result);
}
nlohmann::json presence(const Snapshot& snapshot, double now, const std::optional<Artwork>& artwork) {
    if (snapshot.state != PlaybackState::playing || !snapshot.track || snapshot.track->title.empty()) return nullptr;
    const auto& track = *snapshot.track;
    nlohmann::json payload{{"type", 2}, {"name", "Apple Music"}, {"status_display_type", 1},
        {"details", discord_text(track.title)}, {"state", discord_text(track.artist.empty() ? "Apple Music" : track.artist)}};
    if (snapshot.position && snapshot.duration && std::isfinite(*snapshot.position) && std::isfinite(*snapshot.duration)
        && *snapshot.duration > 0 && std::isfinite(snapshot.playback_rate) && std::abs(snapshot.playback_rate - 1.0) < .01) {
        double position = std::clamp(*snapshot.position + std::max(0.0, now - snapshot.observed_at), 0.0, *snapshot.duration);
        auto start = static_cast<int64_t>(now - position);
        payload["timestamps"] = {{"start", start}, {"end", std::max(start + 1, static_cast<int64_t>(start + *snapshot.duration))}};
    }
    if (artwork) {
        payload["assets"] = {{"large_image", artwork->url}, {"large_text", discord_text(track.album.empty() ? track.title : track.album)}};
        if (!artwork->track_url.empty()) payload["buttons"] = nlohmann::json::array({{{"label", "Listen on Apple Music"}, {"url", artwork->track_url}}});
    }
    return payload;
}
bool materially_changed(const nlohmann::json& before, const nlohmann::json& after) {
    if (before.is_null() || after.is_null()) return before != after;
    auto a = before, b = after; a.erase("timestamps"); b.erase("timestamps");
    if (a != b) return true;
    if (before.contains("timestamps") != after.contains("timestamps")) return true;
    if (!before.contains("timestamps")) return false;
    return std::abs(before["timestamps"]["start"].get<int64_t>() - after["timestamps"]["start"].get<int64_t>()) >= 3
        || std::abs(before["timestamps"]["end"].get<int64_t>() - after["timestamps"]["end"].get<int64_t>()) >= 3;
}
}

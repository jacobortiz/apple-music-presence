#include <amp/presence.hpp>
#include <amp/settings.hpp>
#include <Windows.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>

#ifdef NDEBUG
#error Native regression checks require assertions to be enabled.
#endif

void test_media();
void test_rpc();
void test_artwork();
void test_service();
static void test_core() {
    using namespace amp;
    Snapshot snapshot{Track{"Song", "Artist", "Album"}, PlaybackState::playing, 10.0, 200.0, 1000.0};
    auto payload = presence(snapshot, 1010);
    assert(payload["type"] == 2 && payload["name"] == "Apple Music" && payload["status_display_type"] == 1);
    assert(payload["state"] == "Artist" && payload["timestamps"]["start"] == 990 && payload["timestamps"]["end"] == 1190);
    auto jitter = payload; jitter["timestamps"]["start"] = 991; assert(!materially_changed(payload, jitter));
    jitter["timestamps"]["start"] = 980; assert(materially_changed(payload, jitter));
    snapshot.state = PlaybackState::paused; assert(presence(snapshot, 1010).is_null());
    snapshot.state = PlaybackState::stopped; assert(presence(snapshot, 1010).is_null());
    snapshot.state = PlaybackState::playing; snapshot.position.reset(); assert(!presence(snapshot, 1010).contains("timestamps"));
    snapshot.position = std::numeric_limits<double>::quiet_NaN(); assert(!presence(snapshot, 1010).contains("timestamps"));
    Artwork shared{"https://is1-ssl.mzstatic.com/cover.jpg", ""};
    auto art = presence(snapshot, 1010, shared); assert(art.contains("assets") && !art.contains("buttons"));
    snapshot.track->title = std::string(127, 'x') + "\xf0\x9f\x8e\xb5";
    assert(presence(snapshot, 1010)["details"].get<std::string>() == std::string(127, 'x'));
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    auto directory = std::filesystem::path(temp) / (L"amp-native-core-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    try {
        Settings settings; settings.client_id = "123456789012345678"; settings.country = "GB"; settings.artwork = true;
        save_settings(settings, directory); auto loaded = load_settings(directory);
        assert(loaded.client_id == settings.client_id && loaded.country == "GB" && loaded.artwork);
        std::ofstream bad(directory / L"native_settings.json", std::ios::binary | std::ios::trunc);
        bad << "{\"ignored\":" << std::string(1000, '[') << "[]" << std::string(1000, ']') << "}"; bad.close();
        assert(load_settings(directory).client_id.empty());
        std::filesystem::remove(directory / L"native_settings.json");
        std::ofstream legacy(directory / L"settings.json"); legacy << "{\"client_id\":\"123456789012345678\"}"; legacy.close();
        assert(load_settings(directory).client_id == "123456789012345678");
        settings.client_id = "not-a-token"; bool rejected{};
        try { validate_settings(settings); } catch (...) { rejected = true; } assert(rejected);
    } catch (...) {
        std::filesystem::remove(directory / L"native_settings.json");
        std::filesystem::remove(directory / L"settings.json");
        std::filesystem::remove(directory); throw;
    }
    std::filesystem::remove(directory / L"settings.json");
    std::filesystem::remove(directory);
}
int main() {
    try {
        test_core(); std::cout << "Core settings/presence checks passed\n";
        test_media(); std::cout << "Media checks passed\n";
        test_rpc(); std::cout << "Discord IPC checks passed\n";
        test_artwork(); std::cout << "Artwork checks passed\n";
        test_service(); std::cout << "Background service checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}

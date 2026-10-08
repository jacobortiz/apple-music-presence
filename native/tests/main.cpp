#include <amp/presence.hpp>
#include <amp/settings.hpp>
#include <Windows.h>
#include <Shellapi.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>

#ifdef NDEBUG
#error Native regression checks require assertions to be enabled.
#endif

void test_media();
void test_rpc();
void test_artwork();
void test_service();
void test_catalog();
void test_motion();
void test_artwork_host();
void test_transport();
void test_artwork_worker();
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
int main(int argc, char* argv[]) {
    // Hidden child fixtures exercise actual Windows quoting, pipe capture,
    // timeout and cancellation without a shell, network or external tools.
    if (argc > 1 && std::string_view(argv[1]).starts_with("--child-")) {
        auto mode = std::string_view(argv[1]);
        if (mode == "--child-wait") { Sleep(5000); return 0; }
        if (mode == "--child-overflow") { std::cout << std::string(1024 * 1024 + 1, 'x'); return 0; }
        if (mode == "--child-echo") {
            int count{}; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
            nlohmann::json output; output["arguments"] = nlohmann::json::array();
            for (int i = 2; i < count; ++i) output["arguments"].push_back(amp::utf8(args[i]));
            LocalFree(args);
            bool secrets{};
            for (auto name : {L"APPLE_MUSIC_PRESENCE_GITHUB_TOKEN", L"GITHUB_TOKEN", L"GH_TOKEN", L"AMP_FAKE_SECRET"})
                secrets |= GetEnvironmentVariableW(name, nullptr, 0) != 0;
            output["secrets"] = secrets;
            char buffer[4096]{}; DWORD bytes{};
            ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &bytes, nullptr);
            output["input"] = std::string(buffer, bytes);
            output["interactive"] = GetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", nullptr, 0) != 0;
            std::cout << output.dump(); return 0;
        }
        return 2;
    }
    try {
        test_core(); std::cout << "Core settings/presence checks passed\n";
        test_media(); std::cout << "Media checks passed\n";
        test_rpc(); std::cout << "Discord IPC checks passed\n";
        test_artwork(); std::cout << "Artwork checks passed\n";
        test_catalog(); std::cout << "Apple page and duplicate JPEG checks passed\n";
        test_motion(); std::cout << "Motion parsing and download bounds checks passed\n";
        test_artwork_host(); std::cout << "Public hosting and credential safety checks passed\n";
        test_transport(); std::cout << "HTTP and child cancellation checks passed\n";
        test_artwork_worker(); std::cout << "Asynchronous artwork upgrade checks passed\n";
        test_service(); std::cout << "Background service checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}

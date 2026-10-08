#include <amp/artwork.hpp>
#include <amp/settings.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("amp-motion-worker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { require(std::filesystem::create_directory(directory), "Create isolated artwork worker fixture"); }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove(directory / "motion_cache.json", ignored);
        std::filesystem::remove(directory, ignored);
    }
    void require_clean_cache() {
        auto json = amp::read_json(directory / "motion_cache.json", 128 * 1024);
        require(json && (*json)["repository"] == "example/public-artwork" && (*json)["albums"].is_object(),
                "Persist only valid bounded motion cache JSON");
        for (const auto& file : std::filesystem::directory_iterator(directory))
            require(file.path().filename() == "motion_cache.json", "Atomic cache save leaves no temporary files");
    }
};
struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Event() { require(handle != nullptr, "Create worker synchronization fixture"); }
    ~Event() { CloseHandle(handle); }
};
template<class Predicate> void await(Predicate predicate, const char* message) {
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(5ms);
    }
}
const amp::Track first_track{"First", "Example Artist", "Example Album"};
const amp::Track second_track{"Second", "Example Artist", "Example Album"};
const amp::Track third_track{"Third", "Example Artist", "Third Album"};
std::string page(const std::string& id) { return "https://music.apple.com/us/album/example/" + id; }
amp::Artwork animated(const std::string& id) {
    return {"https://raw.githubusercontent.com/example/public-artwork/" + std::string(40, 'a')
        + "/artwork/motion/" + id + "-abcdef123456.webp", page(id), true};
}
amp::Settings settings() {
    amp::Settings value; value.artwork = true; value.motion_artwork = true;
    value.artwork_repository = "example/public-artwork"; return value;
}
amp::ArtworkDependencies dependencies(std::atomic<unsigned>& requests) {
    amp::ArtworkDependencies hooks; hooks.request_interval = 0ms;
    hooks.http = [&](std::string_view url, std::size_t, HANDLE) {
        require(url.starts_with("https://itunes.apple.com/search?"), "Offline worker never uses real catalog or host HTTP");
        ++requests;
        const amp::Track* track = url.find("term=First%20") != url.npos ? &first_track
            : url.find("term=Second%20") != url.npos ? &second_track : &third_track;
        const std::string id = track == &first_track ? "123" : track == &second_track ? "999" : "777";
        Json song{{"kind", "song"}, {"trackName", track->title}, {"artistName", track->artist},
            {"collectionName", track->album}, {"trackViewUrl", page(id) + "?i=456"},
            {"artworkUrl100", "https://is1-ssl.mzstatic.com/image/thumb/" + id + "/100x100bb.jpg"}};
        return amp::HttpResponse{200, Json{{"results", Json::array({song})}}.dump(), ""};
    };
    return hooks;
}
class JobLog {
public:
    void add(const std::string& title) {
        { std::lock_guard lock(mutex_); titles_.push_back(title); } changed_.notify_all();
    }
    void expect_count(std::size_t count) {
        std::unique_lock lock(mutex_);
        require(changed_.wait_for(lock, 2s, [&] { return titles_.size() >= count; }), "Expected motion job did not start");
    }
    void expect_only(std::vector<std::string> expected) {
        std::unique_lock lock(mutex_);
        const auto count = titles_.size();
        require(!changed_.wait_for(lock, 100ms, [&] { return titles_.size() != count; }), "Obsolete queued motion job unexpectedly started");
        require(titles_ == expected, "Motion queue retains only the selected latest album");
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> titles_;
};
amp::ArtworkDependencies blocked(std::atomic<unsigned>& requests, JobLog& jobs, Event& release) {
    auto hooks = dependencies(requests);
    hooks.motion = [&](const amp::Track& track, std::string_view verified_page, HANDLE stop) -> std::optional<amp::Artwork> {
        jobs.add(track.title);
        if (track == first_track) {
            HANDLE events[]{stop, release.handle};
            auto result = WaitForMultipleObjects(2, events, FALSE, 2000);
            require(result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1, "Blocked fake motion job did not wake");
            if (result == WAIT_OBJECT_0) return {};
        }
        const auto key = amp::artwork_detail::album_key(verified_page);
        require(key.has_value(), "Fake motion gets only verified public album pages");
        return animated(key->substr(key->rfind(':') + 1));
    };
    return hooks;
}
}

void test_artwork_worker() {
    // A blocked converter must not hide static art or require a new song before
    // its completed animation is returned by the non-blocking refresh path.
    {
        Fixture fixture; Event release; JobLog jobs;
        std::atomic<unsigned> requests{}, notifications{};
        amp::ArtworkResolver resolver(settings(), fixture.directory, [&] { ++notifications; }, blocked(requests, jobs, release));
        auto cover = resolver.resolve(first_track);
        require(cover && !cover->animated, "Return static artwork while motion work is blocked");
        jobs.expect_count(1);
        const auto began = std::chrono::steady_clock::now();
        require(!resolver.refresh(first_track) && std::chrono::steady_clock::now() - began < 100ms,
                "Refreshing blocked motion never stalls playback");
        SetEvent(release.handle);
        await([&] { auto result = resolver.refresh(first_track); return result && result->animated; }, "Same-song motion did not become ready");
        await([&] { return notifications.load() >= 2; }, "Motion completion did not wake presence worker");
        cover = resolver.resolve(first_track);
        require(cover && cover->animated && requests == 1, "Same-song cached static lookup upgrades to animation without another catalog call");
        fixture.require_clean_cache();
        jobs.expect_only({"First"});
    }
    // Songs sharing an album name still require their own verified catalog ID.
    {
        Fixture fixture; Event release; JobLog jobs;
        std::atomic<unsigned> requests{}, notifications{};
        amp::ArtworkResolver resolver(settings(), fixture.directory, [&] { ++notifications; }, blocked(requests, jobs, release));
        resolver.resolve(first_track); jobs.expect_count(1); SetEvent(release.handle);
        await([&] { return notifications.load() >= 2; }, "First album cache did not finish");
        require(!resolver.refresh(second_track), "Unverified song cannot reuse identically named album animation");
        auto cover = resolver.resolve(second_track);
        require(cover && !cover->animated && cover->url.find("/999/") != std::string::npos && requests == 2,
                "Verify new song release before any cached album reuse");
        resolver.refresh(second_track); jobs.expect_count(2);
        await([&] { auto result = resolver.refresh(second_track); return result && result->track_url == page("999"); },
              "New song uses its own verified album animation");
        require(resolver.refresh(second_track)->track_url != page("123"), "Never reuse previous release ID based only on album text");
    }
    // Queue one active job and only the latest selected next album.
    {
        Fixture fixture; Event release; JobLog jobs; std::atomic<unsigned> requests{}, notifications{};
        amp::ArtworkResolver resolver(settings(), fixture.directory, [&] { ++notifications; }, blocked(requests, jobs, release));
        resolver.resolve(first_track); jobs.expect_count(1);
        resolver.resolve(second_track); resolver.refresh(second_track);
        resolver.resolve(third_track); resolver.refresh(third_track);
        SetEvent(release.handle); jobs.expect_count(2);
        await([&] { return notifications.load() >= 4; }, "Latest queued album did not complete");
        jobs.expect_only({"First", "Third"});
    }
    // Returning to the active album clears the previously queued album.
    {
        Fixture fixture; Event release; JobLog jobs; std::atomic<unsigned> requests{}, notifications{};
        amp::ArtworkResolver resolver(settings(), fixture.directory, [&] { ++notifications; }, blocked(requests, jobs, release));
        resolver.resolve(first_track); jobs.expect_count(1);
        resolver.resolve(second_track); resolver.refresh(second_track);
        resolver.refresh(first_track);
        resolver.resolve(second_track); // A late obsolete static result cannot reselect its album.
        SetEvent(release.handle);
        await([&] { return notifications.load() >= 2; }, "Active album did not complete after return");
        jobs.expect_only({"First"});
    }
    // Switching to an already cached animation also discards obsolete work.
    {
        Fixture fixture; Event release; JobLog jobs; std::atomic<unsigned> requests{}, notifications{};
        auto motion = animated("777");
        Json cache{{"repository", "example/public-artwork"}, {"encoding_profile", "webp-768-q85-lanczos-v2"},
            {"albums", {{"album:us:777", {{"expires", amp::unix_time() + 1000},
              {"cover", {{"url", motion.url}, {"track_url", motion.track_url}}}}}}}};
        { std::ofstream file(fixture.directory / "motion_cache.json", std::ios::binary); file << cache.dump(); }
        amp::ArtworkResolver resolver(settings(), fixture.directory, [&] { ++notifications; }, blocked(requests, jobs, release));
        resolver.resolve(first_track); jobs.expect_count(1);
        resolver.resolve(second_track); resolver.refresh(second_track);
        resolver.resolve(third_track);
        auto cached = resolver.refresh(third_track);
        require(cached && cached->animated && cached->track_url == page("777") && requests == 3,
                "Verify current song before using persistent album animation");
        SetEvent(release.handle);
        await([&] { return notifications.load() >= 2; }, "Active album did not finish while cached album was selected");
        jobs.expect_only({"First"});
    }
    // Normal artwork survives both verified absence and temporary preparation
    // failures. Repeated refreshes respect the motion negative-cache lifetime.
    for (bool failing : {false, true}) {
        Fixture fixture; std::atomic<unsigned> requests{}, preparations{}, notifications{};
        auto hooks = dependencies(requests);
        hooks.motion = [&](const amp::Track&, std::string_view, HANDLE) -> std::optional<amp::Artwork> {
            ++preparations; if (failing) throw std::runtime_error("Fake optional conversion failure"); return {};
        };
        amp::ArtworkResolver resolver(settings(), fixture.directory, [&] { ++notifications; }, hooks);
        const auto initial = resolver.resolve(first_track);
        await([&] { return notifications.load() >= 2; }, "Negative motion result did not complete");
        const auto current = resolver.resolve(first_track);
        require(initial && current && !current->animated && current->url == initial->url && requests == 1,
                "Optional motion failure or absence preserves normal artwork");
        for (unsigned i = 0; i < 20; ++i) require(!resolver.refresh(first_track), "Negative motion result remains normal art");
        require(preparations == 1, "Motion negative cache prevents repeated conversion");
        require(resolver.status(first_track).find(failing ? "retrying" : "No motion") != std::string::npos,
                "Distinguish temporary motion failure from verified absence");
        fixture.require_clean_cache();
    }
    // A blocked optional job responds to cancellation and leaves no partial
    // cache. Destruction joins it before callback captures leave scope.
    {
        Fixture fixture; Event release; JobLog jobs; std::atomic<unsigned> requests{};
        const auto began = std::chrono::steady_clock::now();
        auto resolver = std::make_unique<amp::ArtworkResolver>(settings(), fixture.directory, std::function<void()>{}, blocked(requests, jobs, release));
        resolver->resolve(first_track); jobs.expect_count(1);
        const auto cancelling = std::chrono::steady_clock::now();
        resolver->cancel(); resolver.reset();
        require(std::chrono::steady_clock::now() - cancelling < 500ms, "Cancel blocked optional motion promptly");
        require(std::chrono::steady_clock::now() - began < 2s && std::filesystem::is_empty(fixture.directory),
                "Cancellation leaves no atomic cache temporary files");
    }
}

#include <amp/artwork_host.hpp>
#include <amp/motion.hpp>

#include <nlohmann/json.hpp>
#include <functional>
#include <stdexcept>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr std::string_view repository = "example/album-art";
constexpr std::string_view stream = "https://mvod.itunes.apple.com/test/main.m3u8";
const std::string sha_a(40, 'a'), sha_b(40, 'b');
const std::string api = "https://api.github.com/repos/" + std::string(repository);
const std::string ref = api + "/git/ref/heads/motion-artwork";
const std::string filename = "123-4a0114610c18.webp";
const std::string path = "artwork/motion/" + filename;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Operation>
void must_fail(Operation operation) {
    bool failed{};
    try { operation(); }
    catch (const std::exception& error) {
        require(std::string_view(error.what()).find("test-only-token") == std::string_view::npos,
                "Upload failure exposed a credential.");
        failed = true;
    }
    require(failed, "Unsafe GitHub artwork operation was accepted.");
}

void put_u32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes[offset++] = static_cast<char>(value >> shift);
}
std::string animation() {
    std::string result("RIFF\0\0\0\0WEBP", 12);
    auto chunk = [&](std::string_view kind, std::string body) {
        result += kind;
        const auto offset = result.size();
        result.append(4, '\0');
        put_u32(result, offset, static_cast<std::uint32_t>(body.size()));
        result += body;
        if (body.size() % 2) result += '\0';
    };
    std::string extended(10, '\0'); extended[0] = 2;
    chunk("VP8X", extended);
    chunk("ANIM", std::string(6, '\0'));
    chunk("ANMF", std::string(16, '\0'));
    chunk("ANMF", std::string(16, '\0'));
    put_u32(result, 4, static_cast<std::uint32_t>(result.size() - 8));
    require(amp::motion_detail::animated_webp(result), "Invalid host test animation fixture.");
    return result;
}

std::string raw(const std::string& sha) {
    return "https://raw.githubusercontent.com/" + std::string(repository) + "/" + sha + "/" + path;
}
amp::HttpResponse response(int status, const Json& body) { return {status, body.dump(), {}}; }
amp::HttpResponse repo(bool public_repo = true, bool writable = true) {
    return response(200, Json{{"private", !public_repo}, {"permissions", {{"push", writable}}}, {"default_branch", "feature/main"}});
}
amp::HttpResponse identity() { return response(200, Json{{"login", "test-user"}, {"id", 12345}}); }
amp::HttpResponse reference(const std::string& sha) {
    return response(200, Json{{"object", {{"type", "commit"}, {"sha", sha}}}});
}

struct Step {
    std::string url, method{"GET"};
    amp::HttpResponse reply;
    std::function<void(const Json&)> check;
};

class Transport {
public:
    std::vector<Step> steps;
    std::size_t index{};
    unsigned credentials{};
    bool public_only{};
    amp::HostHttp http() {
        return [&](std::string_view url, std::size_t limit, std::string_view method,
                   std::string_view body, std::string_view token, HANDLE) {
            require(index < steps.size(), "Unexpected GitHub request.");
            const auto& step = steps[index++];
            require(url == step.url && method == step.method, "Unexpected GitHub URL or mutation.");
            const bool api_request = url.starts_with("https://api.github.com/");
            const bool authenticated = api_request && !public_only;
            require(token == (authenticated ? "test-only-token" : ""), "Credentials escaped the GitHub API.");
            require(limit <= (api_request ? 256 * 1024 : amp::max_motion_image_bytes), "HTTP limit is unbounded.");
            if (step.check) step.check(Json::parse(body));
            else require(body.empty(), "Unexpected public request body.");
            return step.reply;
        };
    }
    amp::CredentialProvider provider() {
        return [&](std::string_view destination, HANDLE) {
            require(destination == repository, "Credential helper received the wrong repository.");
            ++credentials;
            return "test-only-token";
        };
    }
    void finished() { require(index == steps.size(), "Expected GitHub request was skipped."); }
};

void check_upload(const Json& body) {
    require(body.size() == 5 && body.at("branch") == "motion-artwork" && !body.contains("sha"),
            "Artwork upload must only create a file on its dedicated branch.");
    require(body.at("message") == "Cache motion cover for Apple Music album 123", "Upload exposed extra listening metadata.");
    const Json expected{{"name", "test-user"}, {"email", "12345+test-user@users.noreply.github.com"}};
    require(body.at("author") == expected && body.at("committer") == expected,
            "Upload did not use the verified no-reply identity.");
    require(body.at("content") == "UklGRlQAAABXRUJQVlA4WAoAAAACAAAAAAAAAAAAQU5JTQYAAAAAAAAAAABBTk1GEAAAAAAAAAAAAAAAAAAAAAAAAABBTk1GEAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            "Upload did not contain exactly the verified animation bytes.");
}

void test_public_lookup() {
    Transport existing{{{ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {200, animation(), {}}}}};
    existing.public_only = true;
    amp::GithubArtworkHost host(std::string(repository), existing.http(), existing.provider());
    require(host.find_hosted("123", stream) == raw(sha_a), "Public cover lookup did not return an immutable URL.");
    require(existing.credentials == 0, "Public lookup requested credentials.");
    existing.finished();
    for (const auto reply : {amp::HttpResponse{404, "", {}}, amp::HttpResponse{429, "", {}},
                            amp::HttpResponse{200, "invalid JSON", {}}, reference("heads/main"),
                            amp::HttpResponse{200, std::string(256 * 1024 + 1, ' '), {}}}) {
        Transport missing{{{ref, "GET", reply}}};
        missing.public_only = true;
        amp::GithubArtworkHost lookup(std::string(repository), missing.http(), missing.provider());
        require(!lookup.find_hosted("123", stream), "Unavailable or invalid public branch should be a miss.");
        require(missing.credentials == 0, "Failed public lookup requested credentials.");
        missing.finished();
    }
    for (const auto reply : {amp::HttpResponse{404, "", {}}, amp::HttpResponse{200, "static WebP", {}},
                            amp::HttpResponse{200, std::string(amp::max_motion_image_bytes + 1, ' '), {}}}) {
        Transport missing{{{ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", reply}}};
        missing.public_only = true;
        amp::GithubArtworkHost lookup(std::string(repository), missing.http(), missing.provider());
        require(!lookup.find_hosted("123", stream), "Invalid public image should be a miss.");
        missing.finished();
    }
    Transport invalid;
    amp::GithubArtworkHost lookup(std::string(repository), invalid.http(), invalid.provider());
    must_fail([&] { lookup.find_hosted("../123", stream); });
    must_fail([&] { lookup.find_hosted("123", "https://attacker.example/main.m3u8"); });
    const auto stop = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    require(stop != nullptr, "Could not create cancellation event.");
    try {
        must_fail([&] { lookup.find_hosted("123", stream, stop); });
        require(invalid.credentials == 0 && invalid.index == 0, "Invalid/cancelled lookup accessed network or credentials.");
        ResetEvent(stop);
        Transport cancelled{{{ref, "GET", reference(sha_a)}}};
        cancelled.public_only = true;
        const auto fake = cancelled.http();
        amp::GithubArtworkHost during_read(std::string(repository),
            [&](auto url, auto limit, auto method, auto body, auto token, auto event) {
                const auto reply = fake(url, limit, method, body, token, event);
                SetEvent(stop);
                return reply;
            }, cancelled.provider());
        must_fail([&] { during_read.find_hosted("123", stream, stop); });
        cancelled.finished();
    } catch (...) { CloseHandle(stop); throw; }
    CloseHandle(stop);
    amp::GithubArtworkHost failure(std::string(repository), [](auto, auto, auto, auto, auto, auto) -> amp::HttpResponse {
        throw std::runtime_error("test-only-token");
    }, invalid.provider());
    require(!failure.find_hosted("123", stream), "Public transport failure must permit normal preparation.");
    require(invalid.credentials == 0, "Public transport failure accessed credentials.");
}

void test_existing_cover() {
    Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
        {ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {200, animation(), {}}}}};
    amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
    require(host.publish("123", stream, animation()) == raw(sha_a), "Existing immutable cover was not reused.");
    require(transport.credentials == 1, "Credentials were requested more than once.");
    transport.finished();
}

void test_create_branch_and_upload() {
    Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
        {ref, "GET", {404, "", {}}}, {api + "/git/ref/heads/feature%2Fmain", "GET", reference(sha_a)},
        {api + "/git/refs", "POST", reference(sha_a), [](const Json& body) {
            require(body == Json{{"ref", "refs/heads/motion-artwork"}, {"sha", sha_a}}, "Created the wrong branch.");
        }}, {raw(sha_a), "GET", {404, "", {}}},
        {api + "/contents/" + path, "PUT", response(201, Json{{"commit", {{"sha", sha_b}}}}), check_upload},
        {raw(sha_b), "GET", {200, animation(), {}}}}};
    amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
    require(host.publish("123", stream, animation()) == raw(sha_b), "New cover URL did not use its immutable commit.");
    transport.finished();
}

void test_branch_and_upload_races() {
    for (const int conflict : {409, 422}) {
        Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", {404, "", {}}}, {api + "/git/ref/heads/feature%2Fmain", "GET", reference(sha_a)},
            {api + "/git/refs", "POST", {conflict, "", {}}, [](const Json& body) {
                require(body.at("ref") == "refs/heads/motion-artwork", "Wrong conflict branch.");
            }}, {ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {404, "", {}}},
            {api + "/contents/" + path, "PUT", {conflict, "", {}}, check_upload},
            {ref, "GET", reference(sha_b)}, {raw(sha_b), "GET", {200, animation(), {}}}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        require(host.publish("123", stream, animation()) == raw(sha_b), "Concurrent upload did not refetch/verify the new branch head.");
        transport.finished();
    }
}

void test_permissions_and_validation() {
    for (const auto state : {std::pair{false, true}, std::pair{true, false}}) {
        Transport transport{{{api, "GET", repo(state.first, state.second)}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        must_fail([&] { host.publish("123", stream, animation()); });
        transport.finished();
    }
    for (const auto status : {401, 403, 429, 500}) {
        Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {status, "", {}}}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        must_fail([&] { host.publish("123", stream, animation()); });
        transport.finished(); // Only 404 may trigger creating a new public image.
    }
    {
        Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", {403, "", {}}}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        must_fail([&] { host.publish("123", stream, animation()); });
        transport.finished(); // A forbidden branch is not a missing branch.
    }
    {
        Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", reference("heads/main")}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        must_fail([&] { host.publish("123", stream, animation()); });
        transport.finished(); // Never construct a public URL from an unverified SHA.
    }
    for (const auto cover : {std::string("not WebP"), std::string("RIFF malformed")}) {
        Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {200, cover, {}}}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        must_fail([&] { host.publish("123", stream, animation()); });
        transport.finished();
    }
    {
        Transport transport{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {404, "", {}}},
            {api + "/contents/" + path, "PUT", response(201, Json{{"commit", {{"sha", sha_b}}}}), check_upload},
            {raw(sha_b), "GET", {200, "not WebP", {}}}}};
        amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
        must_fail([&] { host.publish("123", stream, animation()); });
        transport.finished(); // A successful PUT is not enough without public byte verification.
    }
    Transport invalid;
    amp::GithubArtworkHost host(std::string(repository), invalid.http(), invalid.provider());
    must_fail([&] { host.publish("../123", stream, animation()); });
    must_fail([&] { host.publish("123", "https://attacker.example/main.m3u8", animation()); });
    must_fail([&] { host.publish("123", stream, "static WebP"); });
    require(invalid.credentials == 0 && invalid.index == 0, "Invalid input accessed credentials or network.");
}

void test_cancel_and_error_privacy() {
    const auto stop = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    require(stop != nullptr, "Could not create cancellation event.");
    Transport transport;
    amp::GithubArtworkHost host(std::string(repository), transport.http(), transport.provider());
    must_fail([&] { host.publish("123", stream, animation(), stop); });
    CloseHandle(stop);
    require(transport.credentials == 0 && transport.index == 0, "Cancelled upload accessed credentials or network.");
    const auto after_read = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    require(after_read != nullptr, "Could not create cancellation event.");
    try {
        Transport before_mutation{{{api, "GET", repo()}, {"https://api.github.com/user", "GET", identity()},
            {ref, "GET", reference(sha_a)}, {raw(sha_a), "GET", {404, "", {}}}}};
        const auto fake = before_mutation.http();
        amp::GithubArtworkHost cancelled_host(std::string(repository),
            [&](auto url, auto limit, auto method, auto body, auto token, auto event) {
                auto result = fake(url, limit, method, body, token, event);
                if (url == raw(sha_a)) SetEvent(after_read);
                return result;
            }, before_mutation.provider());
        must_fail([&] { cancelled_host.publish("123", stream, animation(), after_read); });
        before_mutation.finished();
    } catch (...) { CloseHandle(after_read); throw; }
    CloseHandle(after_read);
    amp::GithubArtworkHost helper_failure(std::string(repository), transport.http(), [](auto, auto) -> std::string {
        throw std::runtime_error("test-only-token");
    });
    must_fail([&] { helper_failure.publish("123", stream, animation()); });
    amp::GithubArtworkHost request_failure(std::string(repository), [](auto, auto, auto, auto, auto, auto) -> amp::HttpResponse {
        throw std::runtime_error("test-only-token");
    }, transport.provider());
    must_fail([&] { request_failure.publish("123", stream, animation()); });
}
} // namespace

void test_artwork_host() {
    require(amp::host_detail::valid_repository(repository), "Valid repository was rejected.");
    for (const auto invalid : {"../repo", "owner/../repo", "https://github.com/owner/repo", "owner/repo?token=value", "owner/repo\n"})
        require(!amp::host_detail::valid_repository(invalid), "Unsafe repository was accepted.");
    require(amp::host_detail::motion_filename("123", stream) == filename, "Keep motion filenames compatible with existing cached hashes.");
    test_existing_cover();
    test_public_lookup();
    test_create_branch_and_upload();
    test_branch_and_upload_races();
    test_permissions_and_validation();
    test_cancel_and_error_privacy();
}

#include <amp/artwork_host.hpp>
#include <amp/motion.hpp>
#include <amp/process.hpp>

#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

namespace amp {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_image = max_motion_image_bytes;
constexpr std::size_t max_api = 256 * 1024;
constexpr std::string_view branch = "motion-artwork";
constexpr std::string_view profile = motion_encoding_profile;

bool alphanumeric(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool valid_token(std::string_view token) {
    return !token.empty() && token.size() <= 4096 &&
        std::all_of(token.begin(), token.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

bool valid_album(std::string_view value) {
    return !value.empty() && value.size() <= 20 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool valid_stream(std::string_view value) {
    return motion_detail::apple_stream(value);
}

bool valid_sha(std::string_view value) {
    return value.size() == 40 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void checkpoint(HANDLE stop) {
    if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
        throw std::runtime_error("Motion cover upload cancelled.");
}

class Secret {
public:
    explicit Secret(std::string value) : value(std::move(value)) {}
    ~Secret() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
    Secret(const Secret&) = delete;
    std::string value;
};

Json parse(std::string_view text, std::size_t limit) {
    if (text.size() > limit) throw std::runtime_error("GitHub artwork response exceeded its size limit.");
    try {
        auto result = Json::parse(text, [](int depth, auto, auto&) {
            if (depth > 64) throw std::runtime_error("GitHub response nesting limit.");
            return true;
        });
        if (result.is_object()) return result;
    } catch (...) {}
    throw std::runtime_error("GitHub returned an invalid artwork response.");
}

std::string field(const Json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : "";
}

std::string ref_sha(const Json& value) {
    const auto object = value.find("object");
    if (object != value.end() && object->is_object() && field(*object, "type") == "commit") {
        const auto sha = field(*object, "sha");
        if (valid_sha(sha)) return sha;
    }
    throw std::runtime_error("GitHub artwork branch could not be verified.");
}

std::string encode_path(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if (alphanumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}

std::string base64(std::string_view input) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto a = static_cast<unsigned char>(input[offset]);
        const auto b = offset + 1 < input.size() ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const auto c = offset + 2 < input.size() ? static_cast<unsigned char>(input[offset + 2]) : 0;
        output += alphabet[a >> 2];
        output += alphabet[((a & 3) << 4) | (b >> 4)];
        output += offset + 1 < input.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        output += offset + 2 < input.size() ? alphabet[c & 63] : '=';
    }
    return output;
}

void retry_wait(HANDLE stop, DWORD delay) {
    if (stop) {
        if (WaitForSingleObject(stop, delay) == WAIT_OBJECT_0) checkpoint(stop);
    } else std::this_thread::sleep_for(std::chrono::milliseconds(delay));
}
} // namespace

namespace host_detail {
bool valid_repository(std::string_view repository) {
    const auto split = repository.find('/');
    if (split == std::string_view::npos || repository.find('/', split + 1) != std::string_view::npos) return false;
    const auto component = [](std::string_view value) {
        return !value.empty() && value.size() <= 100 && alphanumeric(static_cast<unsigned char>(value.front())) &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return alphanumeric(c) || c == '_' || c == '-' || c == '.';
            });
    };
    return component(repository.substr(0, split)) && component(repository.substr(split + 1));
}

std::string motion_filename(std::string_view album_id, std::string_view stream) {
    if (!valid_album(album_id) || !valid_stream(stream)) throw std::runtime_error("Invalid motion cover source.");
    std::string material(profile);
    material += '\0';
    material += stream;
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Motion cover fingerprint unavailable.");
    std::array<unsigned char, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(material.data()), static_cast<ULONG>(material.size()),
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) throw std::runtime_error("Motion cover fingerprint unavailable.");
    constexpr char hex[] = "0123456789abcdef";
    std::string hash;
    for (std::size_t index = 0; index < 6; ++index) {
        hash += hex[digest[index] >> 4];
        hash += hex[digest[index] & 15];
    }
    return std::string(album_id) + "-" + hash + ".webp";
}

std::string github_token(std::string_view repository, HANDLE stop) {
    if (!valid_repository(repository)) throw std::runtime_error("Invalid GitHub artwork repository.");
    checkpoint(stop);
    constexpr auto variable = L"APPLE_MUSIC_PRESENCE_GITHUB_TOKEN";
    const auto needed = GetEnvironmentVariableW(variable, nullptr, 0);
    if (needed > 4097) throw std::runtime_error("GitHub upload credential is invalid.");
    if (needed) {
        std::vector<wchar_t> token(needed, L'\0');
        const auto read = GetEnvironmentVariableW(variable, token.data(), needed);
        std::string result;
        if (read > 0 && read < needed) {
            result.reserve(read);
            for (DWORD index = 0; index < read; ++index) {
                if (token[index] <= 32 || token[index] >= 127) { result.clear(); break; }
                result += static_cast<char>(token[index]);
            }
        }
        SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
        if (!valid_token(result)) throw std::runtime_error("GitHub upload credential is invalid.");
        return result;
    }
    const auto count = SearchPathW(nullptr, L"git.exe", nullptr, 0, nullptr, nullptr);
    if (!count || count > 32768) throw std::runtime_error("GitHub sign-in is required for motion cover uploads.");
    std::vector<wchar_t> executable(count, L'\0');
    if (!SearchPathW(nullptr, L"git.exe", nullptr, count, executable.data(), nullptr))
        throw std::runtime_error("GitHub sign-in is required for motion cover uploads.");
    ProcessResult process;
    try {
        process = run_process(executable.data(), {L"-c", L"credential.interactive=false", L"credential", L"fill"},
            "protocol=https\nhost=github.com\npath=" + std::string(repository) + ".git\n\n",
            std::chrono::seconds(10), stop, true);
    } catch (...) {
        checkpoint(stop);
        throw std::runtime_error("GitHub sign-in is required for motion cover uploads.");
    }
    Secret response(std::move(process.output));
    checkpoint(stop);
    if (process.exit_code != 0) throw std::runtime_error("GitHub sign-in is required for motion cover uploads.");
    std::string password;
    for (std::size_t offset = 0; offset < response.value.size();) {
        const auto newline = response.value.find('\n', offset);
        auto line = std::string_view(response.value).substr(offset,
            newline == std::string::npos ? response.value.size() - offset : newline - offset);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with("password=")) {
            const auto value = line.substr(9);
            if (!password.empty() || !valid_token(value))
                throw std::runtime_error("GitHub credential helper returned invalid credentials.");
            password = value;
        }
        if (newline == std::string::npos) break;
        offset = newline + 1;
    }
    if (!valid_token(password)) throw std::runtime_error("GitHub sign-in is required for motion cover uploads.");
    return password;
}
} // namespace host_detail

GithubArtworkHost::GithubArtworkHost(std::string repository, HostHttp http, CredentialProvider credentials)
    : repository_(std::move(repository)), http_(std::move(http)), credentials_(std::move(credentials)) {
    if (!host_detail::valid_repository(repository_)) throw std::runtime_error("Artwork host must be a GitHub owner/repository.");
    if (!http_) http_ = [](std::string_view url, std::size_t limit, std::string_view method,
                          std::string_view body, std::string_view token, HANDLE stop) {
        return http_request(url, limit, method, body, token, stop);
    };
    if (!credentials_) credentials_ = host_detail::github_token;
}

std::optional<std::string> GithubArtworkHost::find_hosted(std::string_view album_id,
                                                        std::string_view stream, HANDLE stop) {
    const auto filename = host_detail::motion_filename(album_id, stream);
    checkpoint(stop);
    try {
        const auto ref = http_("https://api.github.com/repos/" + repository_ +
            "/git/ref/heads/" + std::string(branch), max_api, "GET", "", "", stop);
        checkpoint(stop);
        if (ref.status != 200) return {};
        const auto sha = ref_sha(parse(ref.body, max_api));
        const auto url = "https://raw.githubusercontent.com/" + repository_ + "/" + sha +
            "/artwork/motion/" + filename;
        const auto image = http_(url, max_image, "GET", "", "", stop);
        checkpoint(stop);
        if (image.status == 200 && image.body.size() <= max_image && motion_detail::animated_webp(image.body))
            return url;
    } catch (...) {
        // Unavailable public reads fall back to normal preparation. Cancellation must not.
        checkpoint(stop);
    }
    return {};
}

std::string GithubArtworkHost::publish(std::string_view album_id, std::string_view stream,
                                      std::string_view content, HANDLE stop) {
    if (!valid_album(album_id) || !valid_stream(stream) || content.size() > max_image ||
        !motion_detail::animated_webp(content)) throw std::runtime_error("Invalid motion cover.");
    checkpoint(stop);
    std::string credential;
    try { credential = credentials_(repository_, stop); }
    catch (...) { checkpoint(stop); throw std::runtime_error("GitHub sign-in is required for motion cover uploads."); }
    Secret token(std::move(credential));
    if (!valid_token(token.value)) throw std::runtime_error("GitHub upload credential is invalid.");
    const std::string base = "https://api.github.com/repos/" + repository_;
    auto request = [&](std::string_view url, std::size_t limit, std::string_view method,
                       std::string_view body, bool authenticated) {
        checkpoint(stop);
        HttpResponse result;
        try { result = http_(url, limit, method, body, authenticated ? std::string_view(token.value) : "", stop); }
        catch (...) { checkpoint(stop); throw std::runtime_error("GitHub artwork request failed."); }
        if (result.body.size() > limit) throw std::runtime_error("GitHub artwork response exceeded its size limit.");
        checkpoint(stop);
        return result;
    };
    auto api = [&](std::string_view url, std::string_view method = "GET", std::string_view body = "") {
        return request(url, max_api, method, body, true);
    };
    auto accepted = [](const HttpResponse& result) {
        if (result.status != 200 && result.status != 201)
            throw std::runtime_error("GitHub artwork request was rejected.");
        return parse(result.body, max_api);
    };
    const auto repository = accepted(api(base));
    const auto permissions = repository.find("permissions");
    if (!repository.contains("private") || repository["private"] != false || permissions == repository.end() ||
        !permissions->is_object() || !permissions->contains("push") || (*permissions)["push"] != true)
        throw std::runtime_error("Artwork host requires a writable public GitHub repository.");
    const auto identity = accepted(request("https://api.github.com/user", 64 * 1024, "GET", "", true));
    const auto login = field(identity, "login");
    const auto id = identity.find("id");
    if (login.empty() || login.size() > 39 || !std::all_of(login.begin(), login.end(), [](unsigned char c) {
            return alphanumeric(c) || c == '-';
        }) || id == identity.end() || !id->is_number_unsigned() || id->get<std::uint64_t>() == 0)
        throw std::runtime_error("GitHub public commit identity could not be verified.");
    const Json committer{{"name", login}, {"email", std::to_string(id->get<std::uint64_t>()) + "+" + login + "@users.noreply.github.com"}};
    const auto ref_url = base + "/git/ref/heads/" + std::string(branch);
    auto ref_response = api(ref_url);
    Json ref;
    if (ref_response.status == 404) {
        const auto default_branch = field(repository, "default_branch");
        if (default_branch.empty() || default_branch.size() > 255 ||
            std::any_of(default_branch.begin(), default_branch.end(), [](unsigned char c) { return c <= 32 || c == 127; }))
            throw std::runtime_error("GitHub default branch could not be verified.");
        const auto parent = ref_sha(accepted(api(base + "/git/ref/heads/" + encode_path(default_branch))));
        ref_response = api(base + "/git/refs", "POST", Json{{"ref", "refs/heads/motion-artwork"}, {"sha", parent}}.dump());
        if (ref_response.status == 409 || ref_response.status == 422) ref_response = api(ref_url);
    }
    ref = accepted(ref_response);
    const auto path = "artwork/motion/" + host_detail::motion_filename(album_id, stream);
    auto public_url = [&](const std::string& sha) {
        if (!valid_sha(sha)) throw std::runtime_error("GitHub artwork commit could not be verified.");
        return "https://raw.githubusercontent.com/" + repository_ + "/" + sha + "/" + path;
    };
    auto url = public_url(ref_sha(ref));
    auto existing = request(url, max_image, "GET", "", false);
    if (existing.status == 200) {
        if (!motion_detail::animated_webp(existing.body)) throw std::runtime_error("Hosted cover is not a verified animated WebP.");
        return url;
    }
    if (existing.status != 404) throw std::runtime_error("Public artwork check failed.");
    const Json upload{{"branch", branch}, {"message", "Cache motion cover for Apple Music album " + std::string(album_id)},
        {"committer", committer}, {"author", committer}, {"content", base64(content)}};
    const auto uploaded = api(base + "/contents/" + path, "PUT", upload.dump());
    if (uploaded.status == 409 || uploaded.status == 422) url = public_url(ref_sha(accepted(api(ref_url))));
    else {
        const auto result = accepted(uploaded);
        const auto commit = result.find("commit");
        if (commit == result.end() || !commit->is_object()) throw std::runtime_error("GitHub artwork commit could not be verified.");
        url = public_url(field(*commit, "sha"));
    }
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        const auto hosted = request(url, max_image, "GET", "", false);
        if (hosted.status == 200) {
            if (!motion_detail::animated_webp(hosted.body)) throw std::runtime_error("Public cover verification failed.");
            return url;
        }
        if (hosted.status != 404 || attempt == 2) throw std::runtime_error("Public cover verification failed.");
        retry_wait(stop, 500 * (attempt + 1));
    }
    throw std::runtime_error("Public cover verification failed.");
}
} // namespace amp

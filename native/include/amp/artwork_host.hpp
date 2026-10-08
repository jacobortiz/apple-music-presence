#pragma once

#include <amp/http.hpp>
#include <functional>
#include <string>
#include <string_view>

namespace amp {
using HostHttp = std::function<HttpResponse(std::string_view url, std::size_t limit,
    std::string_view method, std::string_view body, std::string_view token, HANDLE stop)>;
using CredentialProvider = std::function<std::string(std::string_view repository, HANDLE stop)>;

class GithubArtworkHost {
public:
    explicit GithubArtworkHost(std::string repository, HostHttp http = {},
                               CredentialProvider credentials = {});
    std::string publish(std::string_view album_id, std::string_view stream,
                        std::string_view content, HANDLE stop = nullptr);
    const std::string& repository() const noexcept { return repository_; }
private:
    std::string repository_;
    HostHttp http_;
    CredentialProvider credentials_;
};

namespace host_detail {
bool valid_repository(std::string_view repository);
std::string motion_filename(std::string_view album_id, std::string_view stream);
std::string github_token(std::string_view repository, HANDLE stop = nullptr);
} // namespace host_detail
} // namespace amp

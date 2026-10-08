#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace amp {
struct HttpResponse { int status{}; std::string body, location; };
struct HttpSizeError : std::runtime_error { using std::runtime_error::runtime_error; };
// HTTPS to the public Apple/GitHub endpoints only. Redirects, cookies and
// automatic authentication are disabled; credentials can reach api.github.com only.
HttpResponse http_request(std::string_view url, size_t limit, std::string_view method,
                          std::string_view body, std::string_view github_token, HANDLE stop = nullptr);
HttpResponse http_get(std::string_view url, size_t limit, HANDLE stop = nullptr);
namespace http_detail {
bool permitted_url(std::string_view url, bool authenticated = false);
}
}

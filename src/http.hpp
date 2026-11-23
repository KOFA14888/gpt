#pragma once
// Minimal HTTPS JSON/SSE client.
// Windows: WinHTTP (no external deps). Other OS: libcurl if present, else error.

#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace sm {

struct HttpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// POST JSON body, stream SSE "data:" lines (or whole body if not SSE).
// Returns concatenated text content extracted by on_sse if provided,
// otherwise the raw body. Throws HttpError on transport / HTTP status != 200.
std::string http_post_sse(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    const std::function<void(const std::string& sse_data)>& on_sse = nullptr,
    int connect_timeout_sec = 30,
    int read_timeout_sec = 600);

// GET JSON body (model catalogue). Throws HttpError.
std::string http_get(const std::string& url,
                     const std::map<std::string, std::string>& headers = {},
                     int timeout_sec = 20);

}  // namespace sm

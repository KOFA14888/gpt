// HTTP client implementation — WinHTTP on Windows, libcurl elsewhere.

#include "http.hpp"

#include "common.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#elif !defined(SM_NO_HTTP)
#include <curl/curl.h>
#endif

namespace sm {

#ifdef _WIN32

namespace {

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

struct UrlParts {
    bool https = true;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 443;
};

UrlParts parse_url(const std::string& url) {
    UrlParts u;
    std::string rest = url;
    if (starts_with(rest, "https://")) {
        rest = rest.substr(8);
        u.https = true;
        u.port = 443;
    } else if (starts_with(rest, "http://")) {
        rest = rest.substr(7);
        u.https = false;
        u.port = 80;
    } else {
        throw HttpError("bad url: " + url);
    }
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    u.path = slash == std::string::npos ? L"/" : to_wide(rest.substr(slash));
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        u.host = to_wide(hostport.substr(0, colon));
        u.port = (INTERNET_PORT)std::atoi(hostport.c_str() + colon + 1);
    } else {
        u.host = to_wide(hostport);
    }
    return u;
}

struct WinHttpSession {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    ~WinHttpSession() {
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

std::string winhttp_request(const std::string& method, const std::string& url,
                            const std::map<std::string, std::string>& headers,
                            const std::string& body, int connect_timeout_sec,
                            int read_timeout_sec,
                            const std::function<void(const std::string&)>& on_chunk) {
    UrlParts u = parse_url(url);
    WinHttpSession s;
    s.session = WinHttpOpen(L"ScreenMate/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s.session) throw HttpError("WinHttpOpen failed");
    WinHttpSetTimeouts(s.session, connect_timeout_sec * 1000, connect_timeout_sec * 1000,
                       read_timeout_sec * 1000, read_timeout_sec * 1000);
    s.connection = WinHttpConnect(s.session, u.host.c_str(), u.port, 0);
    if (!s.connection) throw HttpError("WinHttpConnect failed");

    DWORD flags = u.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(s.connection, to_wide(method).c_str(), u.path.c_str(),
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) throw HttpError("WinHttpOpenRequest failed");

    std::wstring hdrs;
    for (auto& [k, v] : headers) hdrs += to_wide(k + ": " + v) + L"\r\n";
    if (!hdrs.empty()) hdrs += L"\r\n";

    BOOL ok;
    if (hdrs.empty()) {
        ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0);
    } else {
        ok = WinHttpSendRequest(req, hdrs.c_str(), (DWORD)-1L,
                                (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0);
    }
    if (!ok) {
        DWORD e = GetLastError();
        WinHttpCloseHandle(req);
        throw HttpError("WinHttpSendRequest failed: " + std::to_string(e));
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        DWORD e = GetLastError();
        WinHttpCloseHandle(req);
        throw HttpError("WinHttpReceiveResponse failed: " + std::to_string(e));
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

    std::string out;
    std::vector<char> buf(16 * 1024);
    DWORD read = 0;
    for (;;) {
        if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &read)) break;
        if (read == 0) break;
        out.append(buf.data(), read);
        if (on_chunk) on_chunk(std::string(buf.data(), read));
    }
    WinHttpCloseHandle(req);

    if (status < 200 || status >= 300) {
        throw HttpError("HTTP " + std::to_string(status) + ": " + out.substr(0, 2000));
    }
    return out;
}

}  // namespace

std::string http_post_sse(const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body,
                          const std::function<void(const std::string&)>& on_sse,
                          int connect_timeout_sec, int read_timeout_sec) {
    return winhttp_request("POST", url, headers, body, connect_timeout_sec, read_timeout_sec,
                           on_sse);
}

std::string http_get(const std::string& url, const std::map<std::string, std::string>& headers,
                     int timeout_sec) {
    return winhttp_request("GET", url, headers, "", timeout_sec, timeout_sec, nullptr);
}

#elif defined(SM_NO_HTTP)

std::string http_post_sse(const std::string&, const std::map<std::string, std::string>&,
                          const std::string&,
                          const std::function<void(const std::string&)>&, int, int) {
    throw HttpError("HTTPS client not built (SM_NO_HTTP). Install libcurl and reconfigure.");
}

std::string http_get(const std::string&, const std::map<std::string, std::string>&, int) {
    throw HttpError("HTTPS client not built (SM_NO_HTTP). Install libcurl and reconfigure.");
}

#else  // POSIX + libcurl

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* fn = static_cast<std::function<void(const std::string&)>*>(userdata);
    size_t n = size * nmemb;
    if (fn && *fn) (*fn)(std::string(ptr, n));
    return n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    (void)buffer;
    return size * nitems;
}

std::string curl_request(const std::string& method, const std::string& url,
                         const std::map<std::string, std::string>& headers,
                         const std::string& body, long timeout_sec,
                         const std::function<void(const std::string&)>& on_chunk) {
    CURL* curl = curl_easy_init();
    if (!curl) throw HttpError("curl_easy_init failed");
    struct curl_slist* hdrs = nullptr;
    for (auto& [k, v] : headers) {
        std::string h = k + ": " + v;
        hdrs = curl_slist_append(hdrs, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    std::function<void(const std::string&)> cb = on_chunk;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);

    std::string errbuf(CURL_ERROR_SIZE, '\0');
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf.data());

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        throw HttpError(std::string("curl: ") +
                        (errbuf.empty() ? curl_easy_strerror(rc) : errbuf.c_str()));
    }
    if (status < 200 || status >= 300) {
        throw HttpError("HTTP " + std::to_string(status));
    }
    return "";
}

}  // namespace

std::string http_post_sse(const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body,
                          const std::function<void(const std::string&)>& on_sse,
                          int, int read_timeout_sec) {
    return curl_request("POST", url, headers, body, read_timeout_sec, on_sse);
}

std::string http_get(const std::string& url, const std::map<std::string, std::string>& headers,
                     int timeout_sec) {
    return curl_request("GET", url, headers, "", timeout_sec, nullptr);
}

#endif

}  // namespace sm

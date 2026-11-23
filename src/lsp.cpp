// Minimal LSP client over stdio (Content-Length framed JSON-RPC).

#include "lsp.hpp"

#include "common.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace sm {

using json = nlohmann::json;

struct LspServer::Impl {
#ifdef _WIN32
    HANDLE stdin_w = nullptr;
    HANDLE stdout_r = nullptr;
    HANDLE process = nullptr;
#else
    int stdin_w = -1;
    int stdout_r = -1;
    pid_t pid = -1;
#endif
    int next_id = 1;
    std::string buffer;
    bool alive = false;

    bool write_msg(const json& j) {
        std::string body = j.dump();
        std::string msg = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
#ifdef _WIN32
        DWORD written = 0;
        return stdin_w && WriteFile(stdin_w, msg.data(), (DWORD)msg.size(), &written, nullptr) &&
               written == msg.size();
#else
        if (stdin_w < 0) return false;
        size_t off = 0;
        while (off < msg.size()) {
            ssize_t n = write(stdin_w, msg.data() + off, msg.size() - off);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
#endif
    }

    bool read_available(int timeout_ms) {
#ifdef _WIN32
        // Peek via WaitForSingleObject on pipe — use PeekNamedPipe loop
        auto deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(stdout_r, nullptr, 0, nullptr, &avail, nullptr)) {
                alive = false;
                return false;
            }
            if (avail > 0) {
                std::vector<char> buf(avail);
                DWORD got = 0;
                if (!ReadFile(stdout_r, buf.data(), avail, &got, nullptr)) {
                    alive = false;
                    return false;
                }
                buffer.append(buf.data(), got);
                return true;
            }
            if (GetTickCount64() >= deadline) return false;
            Sleep(10);
        }
#else
        struct pollfd pfd{stdout_r, POLLIN, 0};
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0) return false;
        if (r == 0) return false;
        if (pfd.revents & (POLLHUP | POLLERR)) {
            alive = false;
            return false;
        }
        char buf[8192];
        ssize_t n = read(stdout_r, buf, sizeof(buf));
        if (n <= 0) {
            alive = false;
            return false;
        }
        buffer.append(buf, (size_t)n);
        return true;
#endif
    }

    // Extract one full JSON-RPC message; returns false if none yet.
    bool pop(json& out) {
        for (;;) {
            auto pos = buffer.find("\r\n\r\n");
            if (pos == std::string::npos) return false;
            size_t len = 0;
            size_t hdr = 0;
            while (hdr < pos) {
                auto eol = buffer.find("\r\n", hdr);
                if (eol == std::string::npos || eol > pos) break;
                std::string line = buffer.substr(hdr, eol - hdr);
                if (starts_with(line, "Content-Length:")) {
                    len = (size_t)std::atol(line.c_str() + 15);
                }
                hdr = eol + 2;
            }
            if (pos + 4 + len > buffer.size()) return false;
            std::string body = buffer.substr(pos + 4, len);
            buffer.erase(0, pos + 4 + len);
            try {
                out = json::parse(body);
                return true;
            } catch (...) {
                // skip malformed
            }
        }
    }

    // Wait until pred(msg) or timeout; ignores notifications that don't match.
    template <typename Pred>
    bool wait_msg(json& out, Pred pred, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            while (pop(out)) {
                if (pred(out)) return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            if (!read_available(50)) {
                if (!alive) return false;
            }
        }
    }
};

LspServer::~LspServer() { shutdown(); }

bool LspServer::running() const { return impl_ && impl_->alive; }

bool LspServer::start(const std::string& command, std::string* error) {
    shutdown();
    impl_ = std::make_unique<Impl>();
    // split command naively on spaces
    std::vector<std::string> parts;
    std::string cur;
    bool in_q = false;
    for (char c : command) {
        if (c == '"') in_q = !in_q;
        else if (c == ' ' && !in_q) {
            if (!cur.empty()) parts.push_back(cur), cur.clear();
        } else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) {
        if (error) *error = "empty lsp command";
        return false;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE, 0};
    HANDLE in_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &impl_->stdin_w, &sa, 0) ||
        !CreatePipe(&impl_->stdout_r, &out_w, &sa, 0)) {
        if (error) *error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(impl_->stdin_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->stdout_r, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline;
    for (auto& p : parts) {
        if (!cmdline.empty()) cmdline += L' ';
        std::wstring w = utf8_to_wide(p);
        if (w.find(L' ') != std::wstring::npos) cmdline += L'"' + w + L'"';
        else cmdline += w;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si,
                             &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (!ok) {
        if (error) *error = "CreateProcess failed for: " + command;
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
#else
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) {
        if (error) *error = "pipe failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        std::vector<char*> argv;
        for (auto& p : parts) argv.push_back(const_cast<char*>(p.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    impl_->stdin_w = in_pipe[1];
    impl_->stdout_r = out_pipe[0];
    impl_->pid = pid;
    fcntl(impl_->stdout_r, F_SETFL, O_NONBLOCK);
#endif
    impl_->alive = true;
    return true;
}

std::string LspServer::initialize(const std::string& root_uri) {
    if (!impl_ || !impl_->alive) return "LSP not running";
    json params = json::object();
    params["processId"] = nullptr;
    params["rootUri"] = root_uri;
    params["capabilities"] = json::object();
    params["capabilities"]["textDocument"] = json::object();
    params["capabilities"]["textDocument"]["definition"] = {{"linkSupport", true}};
    params["capabilities"]["textDocument"]["hover"] = {{"contentFormat", {"markdown", "plaintext"}}};
    params["capabilities"]["workspace"] = {{"applyEdit", true}};

    json req = {{"jsonrpc", "2.0"},
                {"id", impl_->next_id++},
                {"method", "initialize"},
                {"params", params}};
    impl_->write_msg(req);
    impl_->write_msg({{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", json::object()}});

    json resp;
    auto pred = [&](const json& m) { return m.contains("id") && m["id"] == req["id"]; };
    if (!impl_->wait_msg(resp, pred, 8000)) return "LSP initialize timeout";
    if (!resp.contains("result")) return resp.dump();
    return resp["result"].dump();
}

void LspServer::did_open(const std::string& path, const std::string& text) {
    if (!impl_ || !impl_->alive) return;
    std::string uri = "file:///";
    for (char c : path) {
        if (c == '\\') uri += '/';
        else uri += c;
    }
    impl_->write_msg({{"jsonrpc", "2.0"},
                      {"method", "textDocument/didOpen"},
                      {"params",
                       {{"textDocument",
                         {{"uri", uri}, {"languageId", "unknown"}, {"version", 1},
                          {"text", text}}}}}});
}

static json lsp_position_req(LspServer::Impl* impl, const char* method, const std::string& path,
                             const std::string& text, int line, int character) {
    std::string uri = "file:///";
    for (char c : path) {
        if (c == '\\') uri += '/';
        else uri += c;
    }
    json req = {{"jsonrpc", "2.0"},
                {"id", impl->next_id++},
                {"method", method},
                {"params",
                 {{"textDocument", {{"uri", uri}}},
                  {"position", {{"line", line}, {"character", character}}}}}};
    {
        std::string uri2 = "file:///";
        for (char c : path) {
            if (c == '\\') uri2 += '/';
            else uri2 += c;
        }
        impl->write_msg({{"jsonrpc", "2.0"},
                         {"method", "textDocument/didOpen"},
                         {"params",
                          {{"textDocument",
                            {{"uri", uri2}, {"languageId", "unknown"}, {"version", 1},
                             {"text", text}}}}}});
    }
    impl->write_msg(req);
    json resp;
    auto pred = [&](const json& m) { return m.contains("id") && m["id"] == req["id"]; };
    if (!impl->wait_msg(resp, pred, 5000)) return json{{"error", std::string(method) + " timeout"}};
    if (resp.contains("error")) return resp["error"];
    return resp.value("result", json());
}

std::string LspServer::definition(const std::string& path, const std::string& text, int line,
                                  int character) {
    if (!impl_ || !impl_->alive) return "LSP not running";
    return lsp_position_req(impl_.get(), "textDocument/definition", path, text, line, character)
        .dump();
}

std::string LspServer::hover(const std::string& path, const std::string& text, int line,
                             int character) {
    if (!impl_ || !impl_->alive) return "LSP not running";
    return lsp_position_req(impl_.get(), "textDocument/hover", path, text, line, character).dump();
}

void LspServer::shutdown() {
    if (!impl_) return;
    if (impl_->alive) {
        json req = {{"jsonrpc", "2.0"}, {"id", impl_->next_id++}, {"method", "shutdown"}};
        impl_->write_msg(req);
        impl_->write_msg({{"jsonrpc", "2.0"}, {"method", "exit"}});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
#ifdef _WIN32
        if (impl_->process) {
            TerminateProcess(impl_->process, 0);
            CloseHandle(impl_->process);
        }
        if (impl_->stdin_w) CloseHandle(impl_->stdin_w);
        if (impl_->stdout_r) CloseHandle(impl_->stdout_r);
#else
        if (impl_->pid > 0) {
            kill(impl_->pid, SIGTERM);
            int st = 0;
            waitpid(impl_->pid, &st, WNOHANG);
        }
        if (impl_->stdin_w >= 0) close(impl_->stdin_w);
        if (impl_->stdout_r >= 0) close(impl_->stdout_r);
#endif
    }
    impl_.reset();
}

LspServer& lsp_global_server() {
    static LspServer* s = new LspServer();
    return *s;
}

std::string lsp_command_for(const std::map<std::string, std::string>& servers,
                            const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string ext = path.substr(pos);
    auto it = servers.find(ext);
    if (it != servers.end()) return it->second;
    // common defaults if user configured bare names
    static const std::map<std::string, std::string> fallback = {
        {".py", "pylsp"},          {".js", "typescript-language-server --stdio"},
        {".ts", "typescript-language-server --stdio"}, {".cpp", "clangd"},
        {".c", "clangd"},          {".go", "gopls"},
        {".rs", "rust-analyzer"},  {".java", "jdtls"},
    };
    auto fb = fallback.find(ext);
    if (fb != fallback.end() && servers.count("*")) return fb->second;
    return "";
}

}  // namespace sm

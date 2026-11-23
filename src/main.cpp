// ScreenMate AI — console coding agent (Claude Code-style).
// Commands:
//   screenmate                interactive terminal UI (default)
//   screenmate run "prompt"   one-shot agent turn (prints answer)
//   screenmate auth login     save API keys interactively
//   screenmate serve          headless HTTP server (POST /chat)
//   screenmate web            web GUI
//   screenmate models         list models for current provider

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"
#include "http.hpp"
#include "tui.hpp"
#include "tools.hpp"
#include "desktop.hpp"
#include "webui.hpp"

#ifdef _WIN32
// sockets/windows come from common.hpp (winsock2 before windows.h)
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

using namespace sm;
using json = nlohmann::json;

static void print_usage() {
    std::cout <<
        "ScreenMate AI — console coding agent (Claude Code-style)\n"
        "\n"
        "  screenmate                 terminal UI (default): chat, /commands, Tab=build/plan\n"
        "  screenmate run \"prompt\"    one-shot: agent works, prints answer\n"
        "  screenmate run -p \"...\"    same (plan/read-only mode)\n"
        "  screenmate auth login      enter provider + API key interactively\n"
        "  screenmate auth status     show provider + model\n"
        "  screenmate web [--port N]  web GUI\n"
        "  screenmate desktop         native desktop window (Windows)\n"
        "  screenmate serve [--port N]  headless API: POST /chat\n"
        "  screenmate models          list models for current provider\n"
        "  screenmate help\n";
}

static int cmd_auth() {
    Config c = Config::load();
    const auto& defs = provider_defs();

    std::cout << "Providers:\n";
    for (size_t i = 0; i < defs.size(); ++i) {
        std::string mark = defs[i].id == c.provider ? " (current)" : "";
        std::string keym = c.has_key(defs[i].id) ? "key: set" : "key: -";
        std::cout << "  [" << (i + 1) << "] " << defs[i].label << "  " << keym << mark << "\n";
    }
    std::cout << "Provider number: ";
    std::string line;
    std::getline(std::cin, line);
    line = trim(line);
    int idx = atoi(line.c_str());
    if (idx >= 1 && idx <= (int)defs.size()) c.provider = defs[idx - 1].id;

    const auto* d = c.def();
    std::cout << "API key for " << d->label << " (enter to skip, current "
              << (c.has_key(c.provider) ? "set" : "empty") << "): ";
    std::getline(std::cin, line);
    if (!trim(line).empty()) c.keys[c.provider] = trim(line);

    if (c.provider == "custom") {
        std::cout << "Base URL [" << (c.custom_base_url.empty() ? "https://…/v1"
                                                                : c.custom_base_url)
                  << "]: ";
        std::getline(std::cin, line);
        if (!trim(line).empty()) c.custom_base_url = trim(line);
    }

    std::cout << "Model [" << c.model() << "]: ";
    std::getline(std::cin, line);
    if (!trim(line).empty()) c.models[c.provider] = trim(line);

    c.save();
    std::cout << "Saved to " << Config::config_file().string() << "\n";
    if (c.api_key().empty()) {
        std::cout << "Warning: no key for current provider.\n";
        return 1;
    }
    return 0;
}

static int cmd_auth_status() {
    Config c = Config::load();
    std::cout << "provider: " << c.provider << "\n"
              << "model:    " << c.model() << "\n"
              << "key:      " << (c.api_key().empty() ? "(not set)" : "(set)") << "\n"
              << "config:   " << Config::config_file().string() << "\n";
    return 0;
}

static int cmd_run(const std::vector<std::string>& args) {
    bool plan = false;
    std::string prompt;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-p" || args[i] == "--plan") plan = true;
        else if (args[i] == "-m" && i + 1 < args.size()) {
            Config tmp = Config::load();
            (void)tmp;
            // model override handled below via env-style
        } else {
            if (!prompt.empty()) prompt += " ";
            prompt += args[i];
        }
    }
    if (prompt.empty()) {
        std::cerr << "usage: screenmate run [ -p ] \"prompt\"\n";
        return 2;
    }
    Config cfg = Config::load();
    if (cfg.api_key().empty()) {
        std::cerr << "No API key. Run: screenmate auth login\n";
        return 1;
    }
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.mode = plan ? AgentMode::Plan : AgentMode::Build;
    ctx.cwd = fs::current_path();

    std::vector<json> history;
    AgentCallbacks cb;
    std::mutex out_mu;
    cb.on_text = [&](const std::string& d) {
        std::lock_guard<std::mutex> lock(out_mu);
        std::cout << d << std::flush;
    };
    cb.on_status = [&](const std::string& s) {
        std::lock_guard<std::mutex> lock(out_mu);
        std::cerr << "\n[" << s << "]\n";
    };

    try {
        std::string ans = agent_turn(cfg, ctx, history, prompt, cb,
                                     plan ? "plan" : "build");
        if (!ans.empty() && ans.back() != '\n') std::cout << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

static int cmd_models() {
    Config c = Config::load();
    std::map<std::string, std::string> hdrs;
    std::string st = c.style();
    try {
        std::string body;
        if (st == "anthropic") {
            if (!c.api_key().empty()) {
                hdrs["x-api-key"] = c.api_key();
                hdrs["anthropic-version"] = "2023-06-01";
            }
            body = http_get("https://api.anthropic.com/v1/models", hdrs, 20);
        } else if (st == "gemini") {
            if (!c.api_key().empty()) hdrs["x-goog-api-key"] = c.api_key();
            body = http_get(c.base_url() + "/models", hdrs, 20);
        } else {
            if (!c.api_key().empty()) hdrs["Authorization"] = "Bearer " + c.api_key();
            body = http_get(c.base_url() + "/models", hdrs, 20);
        }
        json j = json::parse(body);
        int n = 0;
        json arr = j.value("data", json::array());
        if (arr.empty()) arr = j.value("models", json::array());
        for (auto& m : arr) {
            std::string id = m.value("id", m.value("name", ""));
            if (id.empty()) continue;
            if (id.rfind("models/", 0) == 0) id = id.substr(7);
            std::cout << id << "\n";
            if (++n >= 300) break;
        }
        std::cerr << "(" << n << " models — put id into Settings / config.json model field)\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// ---- tiny HTTP server for `serve` ----
static int cmd_serve(const std::vector<std::string>& args) {
    int port = 8765;
    for (size_t i = 0; i < args.size(); ++i)
        if ((args[i] == "--port" || args[i] == "-p") && i + 1 < args.size())
            port = std::atoi(args[i + 1].c_str());

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind failed on port " << port << "\n";
        return 1;
    }
    listen(srv, 8);
    std::cout << "ScreenMate serve on http://127.0.0.1:" << port
              << "  POST /chat {\"message\":\"...\",\"mode\":\"build|plan\"}\n";

    Config cfg = Config::load();
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.cwd = fs::current_path();
    std::mutex mu;

    for (;;) {
        int c = (int)accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        std::string req;
        char buf[4096];
        // read headers
        size_t hdr_end = std::string::npos;
        while (hdr_end == std::string::npos) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, (size_t)n);
            hdr_end = req.find("\r\n\r\n");
        }
        if (hdr_end == std::string::npos) {
            closesocket(c);
            continue;
        }
        size_t content_len = 0;
        {
            auto pos = req.find("Content-Length:");
            if (pos == std::string::npos) pos = req.find("content-length:");
            if (pos != std::string::npos)
                content_len = (size_t)std::atol(req.c_str() + pos + 15);
        }
        std::string body = req.substr(hdr_end + 4);
        while (body.size() < content_len) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            body.append(buf, (size_t)n);
        }

        std::string message, mode = "build";
        try {
            json j = json::parse(body.empty() ? "{}" : body);
            message = j.value("message", j.value("prompt", ""));
            mode = j.value("mode", "build");
        } catch (...) {
        }

        std::string reply, err;
        if (message.empty()) {
            err = "missing message";
        } else if (cfg.api_key().empty()) {
            err = "no API key — screenmate auth login";
        } else {
            std::lock_guard<std::mutex> lock(mu);
            ToolContext lctx = ctx;
            lctx.mode = (mode == "plan") ? AgentMode::Plan : AgentMode::Build;
            std::vector<json> hist;
            AgentCallbacks cb;
            try {
                reply = agent_turn(cfg, lctx, hist, message, cb,
                                   lctx.mode == AgentMode::Plan ? "plan" : "build");
            } catch (const std::exception& e) {
                err = e.what();
            }
        }

        json out;
        if (err.empty()) out["reply"] = reply;
        else out["error"] = err;
        std::string payload = out.dump();
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "Access-Control-Allow-Origin: *\r\nContent-Length: " +
                           std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
        send(c, resp.data(), (int)resp.size(), 0);
        closesocket(c);
    }
    // unreachable
    // return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // UTF-8 console
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    if (args.empty()) {
        // default: console UI (Claude Code-style)
        Config cfg = Config::load();
        TuiApp app(std::move(cfg));
        return app.run();
    }

    const std::string& cmd = args[0];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }
    if (cmd == "web") {
        std::vector<std::string> rest(args.begin() + 1, args.end());
        int port = 8765;
        for (size_t i = 0; i < rest.size(); ++i)
            if ((rest[i] == "--port" || rest[i] == "-p") && i + 1 < rest.size())
                port = std::atoi(rest[i + 1].c_str());
        if (!rest.empty() && rest[0].rfind("--port", 0) != 0 && rest[0] != "-p" &&
            std::atoi(rest[0].c_str()) > 0)
            port = std::atoi(rest[0].c_str());
        return gui_run(port);
    }
    if (cmd == "desktop" || cmd == "gui") {
        int rc = desktop_run();
        if (rc >= 0) return rc;
        Config cfg = Config::load();
        TuiApp app(std::move(cfg));
        return app.run();
    }
    if (cmd == "tui" || cmd == "term" || cmd == "cli") {
        Config cfg = Config::load();
        TuiApp app(std::move(cfg));
        return app.run();
    }
    if (cmd == "auth") {
        if (args.size() >= 2 && args[1] == "status") return cmd_auth_status();
        if (args.size() >= 2 && args[1] == "login") return cmd_auth();
        // default to login
        return cmd_auth();
    }
    if (cmd == "run") {
        std::vector<std::string> rest(args.begin() + 1, args.end());
        return cmd_run(rest);
    }
    if (cmd == "models") return cmd_models();
    if (cmd == "serve") {
        std::vector<std::string> rest(args.begin() + 1, args.end());
        return cmd_serve(rest);
    }
    // `screenmate "some prompt"` shortcut
    return cmd_run(args);
}

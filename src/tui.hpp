#pragma once
// Claude Code-style terminal UI: transcript, streaming, slash commands, build/plan (Tab).

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "tools.hpp"

namespace sm {

struct UiLine {
    std::string kind;  // user | ai | sys | status | err
    std::string text;
};

class TuiApp {
public:
    explicit TuiApp(Config cfg);
    ~TuiApp();

    int run();  // blocks until exit; returns process exit code

private:
    Config cfg_;
    ToolContext ctx_;
    AgentMode mode_ = AgentMode::Build;
    std::vector<nlohmann::json> history_;
    std::vector<UiLine> lines_;
    std::mutex mu_;
    std::string input_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> cancel_{false};
    std::string status_;
    std::string stream_buf_;  // live streamed answer text

    // terminal
    bool raw_ = false;
    int orig_flags_ = 0;
#ifdef _WIN32
    void* orig_in_mode_ = nullptr;
    unsigned long orig_in_mode_val_ = 0;
    unsigned long orig_out_mode_val_ = 0;
#endif

    void setup_terminal();
    void restore_terminal();
    void draw();
    void push_line(const std::string& kind, const std::string& text);
    void set_status(const std::string& s);
    void handle_key(const std::string& utf8_key, bool ctrl);
    void submit();
    void run_agent_async(std::string text);
    void cmd_model(const std::string& arg);
    void cmd_cwd(const std::string& arg);
    void cmd_tools();
};

}  // namespace sm

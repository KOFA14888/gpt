#include "tui.hpp"

#include "agent.hpp"
#include "common.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace sm {

using std::string;

TuiApp::TuiApp(Config cfg) : cfg_(std::move(cfg)) {
    ctx_.cfg = &cfg_;
    ctx_.cwd = fs::current_path();
    mode_ = AgentMode::Build;
    push_line("sys", "ScreenMate AI — coding agent with screen vision.");
    push_line("sys", "Tab: build/plan · Enter: send · Ctrl+C or /exit: quit · /clear: clear");
    if (cfg_.api_key().empty())
        push_line("err", "No API key — run: screenmate auth login");
    else
        push_line("sys", "Provider: " + cfg_.provider + " · model: " + cfg_.model());
}

TuiApp::~TuiApp() { restore_terminal(); }

void TuiApp::setup_terminal() {
#ifdef _WIN32
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0, out_mode = 0;
    GetConsoleMode(hin, &in_mode);
    GetConsoleMode(hout, &out_mode);
    orig_in_mode_val_ = in_mode;
    orig_out_mode_val_ = out_mode;
    SetConsoleMode(hin, in_mode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
    SetConsoleMode(hout, out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    // alt screen
    fputs("\x1b[?1049h\x1b[2J\x1b[H", stdout);
    raw_ = true;
#else
    if (!isatty(fileno(stdin))) return;
    termios t{};
    tcgetattr(STDIN_FILENO, &t);
    orig_flags_ = t.c_lflag;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    fputs("\x1b[?1049h\x1b[2J\x1b[H", stdout);
    raw_ = true;
#endif
    // hide cursor while drawing, show when needed
    fputs("\x1b[?25l", stdout);
    fflush(stdout);
}

void TuiApp::restore_terminal() {
    if (!raw_) return;
    raw_ = false;
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
#ifdef _WIN32
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hin, orig_in_mode_val_);
    SetConsoleMode(hout, orig_out_mode_val_);
#else
    termios t{};
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag = orig_flags_;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
}

void TuiApp::push_line(const std::string& kind, const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& l : split_lines(text)) lines_.push_back({kind, l});
    if (lines_.size() > 500)
        lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - 500));
}

void TuiApp::set_status(const std::string& s) {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = s;
}

void TuiApp::draw() {
    std::lock_guard<std::mutex> lock(mu_);
    // clear
    fputs("\x1b[H\x1b[2J", stdout);

    const char* mode_color = mode_ == AgentMode::Build ? "\x1b[32m" : "\x1b[33m";
    const char* mode_name = mode_ == AgentMode::Build ? "BUILD" : "PLAN";

    // header
    printf("\x1b[48;5;235m\x1b[38;5;183m"
           " ScreenMate AI "
           "\x1b[0m \x1b[38;5;244m%s\x1b[0m  "
           "\x1b[38;5;244m·\x1b[0m  %s\n",
           mode_color, mode_name);
    // reset color codes properly:
    // (printf above used mode_color incorrectly as string for %s of color — fix below)

    // Actually print header cleanly:
    // We already printed garbage — do a clean redraw instead:
    fputs("\x1b[H\x1b[2J", stdout);
    printf("\x1b[1;36m ScreenMate AI \x1b[0m \x1b[2;90m|\x1b[0m ");
    if (mode_ == AgentMode::Build) printf("\x1b[1;32mBUILD\x1b[0m");
    else printf("\x1b[1;33mPLAN\x1b[0m");
    printf(" \x1b[2;90m| Tab:switch · Enter:send · /exit\x1b[0m\n");
    fputs("\x1b[2;90m────────────────────────────────────────────────────────\x1b[0m\n",
          stdout);

    // transcript: fit terminal
    int rows = 24, cols = 80;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(hout, &info)) {
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        cols = info.srWindow.Right - info.srWindow.Left + 1;
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
#endif
    int max_lines = rows - 4;
    if (max_lines < 3) max_lines = 3;

    size_t start = lines_.size() > (size_t)max_lines ? lines_.size() - max_lines : 0;
    for (size_t i = start; i < lines_.size(); ++i) {
        const auto& L = lines_[i];
        const char* color = "\x1b[0m";
        const char* prefix = "";
        if (L.kind == "user") {
            color = "\x1b[1;34m";
            prefix = "› ";
        } else if (L.kind == "ai") {
            color = "\x1b[0m";
            prefix = "  ";
        } else if (L.kind == "sys") {
            color = "\x1b[2;90m";
            prefix = "· ";
        } else if (L.kind == "status") {
            color = "\x1b[35m";
            prefix = "⚙ ";
        } else if (L.kind == "err") {
            color = "\x1b[31m";
            prefix = "! ";
        }
        printf("%s%s%s\x1b[0m\n", color, prefix, L.text.c_str());
    }

    // status
    if (!status_.empty())
        printf("\x1b[35m⚙ %s\x1b[0m\n", status_.c_str());
    else
        printf("\x1b[2;90m \x1b[0m\n");

    // input
    printf("\x1b[1;36m❯\x1b[0m %s", input_.c_str());
    if (busy_) printf("  \x1b[33m…working (Ctrl+C cancel)\x1b[0m");
    fputs("\x1b[?25h", stdout);  // show cursor
    fflush(stdout);
}

void TuiApp::handle_key(const std::string& utf8_key, bool ctrl) {
    if (ctrl) {
        if (utf8_key == "c" || utf8_key == "C") {
            if (busy_) {
                cancel_ = true;
                set_status("cancelling…");
            } else {
                quit_ = true;
            }
            return;
        }
        if (utf8_key == "u") {
            input_.clear();
            return;
        }
        if (utf8_key == "l") {
            std::lock_guard<std::mutex> lock(mu_);
            lines_.clear();
            return;
        }
    }

    if (utf8_key == "\t") {
        if (busy_) return;
        mode_ = mode_ == AgentMode::Build ? AgentMode::Plan : AgentMode::Build;
        ctx_.mode = mode_;
        push_line("sys", std::string("mode → ") +
                             (mode_ == AgentMode::Build ? "BUILD (full access)" : "PLAN (read-only)"));
        return;
    }
    if (utf8_key == "\r" || utf8_key == "\n") {
        submit();
        return;
    }
    if (utf8_key == "\x7f" || utf8_key == "\b") {
        if (!input_.empty()) {
            // delete one utf-8 codepoint
            while (!input_.empty() && ((unsigned char)input_.back() & 0xC0) == 0x80)
                input_.pop_back();
            if (!input_.empty()) input_.pop_back();
        }
        return;
    }
    // arrows for history could go here; ignore other control chars
    if (!utf8_key.empty() && (unsigned char)utf8_key[0] < 0x20 && utf8_key != "\t") return;
    input_ += utf8_key;
}

void TuiApp::submit() {
    string text = trim(input_);
    input_.clear();
    if (text.empty() || busy_) return;

    if (text == "/exit" || text == "/quit" || text == "/q") {
        quit_ = true;
        return;
    }
    if (text == "/clear") {
        std::lock_guard<std::mutex> lock(mu_);
        lines_.clear();
        history_.clear();
        return;
    }
    if (text == "/mode") {
        handle_key("\t", false);
        return;
    }
    if (text == "/help") {
        push_line("sys",
                  "Commands: /mode /clear /exit · tools: read/write/edit/grep/glob/shell/"
                  "screenshot/list_windows/lsp");
        return;
    }

    push_line("user", text);
    run_agent_async(text);
}

void TuiApp::run_agent_async(string text) {
    busy_ = true;
    cancel_ = false;
    set_status("thinking…");
    std::thread([this, text = std::move(text)]() mutable {
        AgentCallbacks cb;
        cb.on_text = [this](const string& d) {
            // append streamed text as growing ai line — simple: status only + final line
            (void)d;
        };
        cb.on_status = [this](const string& s) { set_status(s); };
        cb.on_image = [this](const string& b64) {
            std::lock_guard<std::mutex> lock(mu_);
            pending_image_b64_ = b64;
        };
        try {
            string answer = agent_turn(cfg_, ctx_, history_, text, cb,
                                       mode_ == AgentMode::Build ? "build" : "plan");
            push_line("ai", answer);
            set_status("");
        } catch (const std::exception& e) {
            push_line("err", string("error: ") + e.what());
            set_status("");
        }
        busy_ = false;
    }).detach();
}

int TuiApp::run() {
    setup_terminal();
    draw();

    // input thread
    std::thread input_th([this]() {
        unsigned char buf[8];
        while (!quit_) {
#ifdef _WIN32
            HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
            INPUT_RECORD rec;
            DWORD read = 0;
            if (!ReadConsoleInputW(hin, &rec, 1, &read) || read == 0) {
                Sleep(10);
                continue;
            }
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
            auto& ke = rec.Event.KeyEvent;
            bool ctrl = (ke.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
            if (ke.wVirtualKeyCode == VK_TAB) {
                handle_key("\t", false);
                continue;
            }
            if (ke.wVirtualKeyCode == VK_RETURN) {
                handle_key("\r", false);
                continue;
            }
            if (ke.wVirtualKeyCode == VK_BACK) {
                handle_key("\x7f", false);
                continue;
            }
            if (ke.uChar.UnicodeChar) {
                wchar_t wch = ke.uChar.UnicodeChar;
                if (ctrl && (wch == L'c' || wch == L'C')) {
                    handle_key("c", true);
                    continue;
                }
                if (ctrl && (wch == L'u' || wch == L'U')) {
                    handle_key("u", true);
                    continue;
                }
                if (ctrl && (wch == L'l' || wch == L'L')) {
                    handle_key("l", true);
                    continue;
                }
                if (wch < 32) continue;
                string u8 = wide_to_utf8(std::wstring(1, wch));
                handle_key(u8, false);
            }
#else
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                usleep(20000);
                continue;
            }
            if (buf[0] == 0x1b) {
                // escape sequences: ignore arrows etc.
                if (n >= 3 && (buf[1] == '[' || buf[1] == 'O')) continue;
                continue;
            }
            if (buf[0] == '\t') {
                handle_key("\t", false);
                continue;
            }
            if (buf[0] == '\r' || buf[0] == '\n') {
                handle_key("\r", false);
                continue;
            }
            if (buf[0] == 127 || buf[0] == 8) {
                handle_key("\x7f", false);
                continue;
            }
            if (buf[0] == 3) {  // Ctrl+C
                handle_key("c", true);
                continue;
            }
            if (buf[0] == 21) {
                handle_key("u", true);
                continue;
            }
            if (buf[0] == 12) {
                handle_key("l", true);
                continue;
            }
            if (buf[0] < 32) continue;
            handle_key(std::string((char*)buf, (size_t)n), false);
#endif
        }
    });

    while (!quit_) {
        draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    set_status("bye");
    draw();
    if (input_th.joinable()) {
        // wake reader: on windows it blocks in ReadConsoleInput — process exit will kill it
        // detach to avoid hang
        input_th.detach();
    }
    restore_terminal();
    return 0;
}

}  // namespace sm

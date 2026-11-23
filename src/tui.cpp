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

// Black & white only: SGR 0=reset 1=bold 2=dim 7=reverse. No colors.
namespace {
inline const char* B = "\x1b[1m";
inline const char* D = "\x1b[2m";
inline const char* R = "\x1b[7m";
inline const char* N = "\x1b[0m";
}  // namespace

TuiApp::TuiApp(Config cfg) : cfg_(std::move(cfg)) {
    ctx_.cfg = &cfg_;
    ctx_.cwd = fs::current_path();
    mode_ = AgentMode::Build;
    push_line("sys", "ScreenMate — консольный кодинг-агент (Claude Code-стиль).");
    push_line("sys", "Tab: build/plan · Enter: отправить · /help: команды · Ctrl+C: выход");
    if (cfg_.api_key().empty())
        push_line("err", "Нет API-ключа — выполни: screenmate auth login");
    else
        push_line("sys", string("Провайдер: ") + cfg_.provider + " · модель: " + cfg_.model() +
                             " · папка: " + ctx_.cwd.string());
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
    fputs("\x1b[H\x1b[2J", stdout);

    const char* mode_tag = mode_ == AgentMode::Build ? "BUILD" : "PLAN ";
    const char* other_tag = mode_ == AgentMode::Build ? "plan" : "build";

    // header: [BUILD] plan  ·  model · cwd
    printf("%s%s%s %s%s%s  %s%s%s%s\n", B, R, mode_tag, N, D, other_tag, N, D,
           cfg_.model().c_str(), " · ", ctx_.cwd.string().c_str());
    printf("%s%s%s\n", D, "────────────────────────────────────────────────────────────", N);

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
    (void)cols;
    int max_lines = rows - 5;
    if (max_lines < 3) max_lines = 3;

    // live streamed text while the agent works (last lines only)
    int stream_extra = 0;
    if (busy_ && !stream_buf_.empty()) {
        auto sl = split_lines(stream_buf_);
        size_t from = sl.size() > 6 ? sl.size() - 6 : 0;
        for (size_t i = from; i < sl.size(); ++i) {
            printf("  %s\n", sl[i].c_str());
            ++stream_extra;
        }
    }

    size_t room = (size_t)std::max(3, max_lines - stream_extra);
    size_t start = lines_.size() > room ? lines_.size() - room : 0;
    for (size_t i = start; i < lines_.size(); ++i) {
        const auto& L = lines_[i];
        if (L.kind == "user") {
            printf("%s❯%s %s\n", B, N, L.text.c_str());
        } else if (L.kind == "ai") {
            printf("  %s\n", L.text.c_str());
        } else if (L.kind == "sys") {
            printf("%s· %s%s\n", D, L.text.c_str(), N);
        } else {
            printf("%s! %s%s\n", B, L.text.c_str(), N);
        }
    }

    // status
    if (!status_.empty())
        printf("%s… %s%s\n", D, status_.c_str(), N);
    else
        printf("%s \n", D);

    // input
    printf("%s❯%s %s", B, N, input_.c_str());
    if (busy_) printf("%s  …работаю (Ctrl+C — отмена)%s", D, N);
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
}

void TuiApp::handle_key(const std::string& utf8_key, bool ctrl) {
    if (ctrl) {
        if (utf8_key == "c" || utf8_key == "C") {
            if (busy_) {
                cancel_ = true;
                set_status("отмена…");
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
        push_line("sys", std::string("режим → ") +
                             (mode_ == AgentMode::Build ? "BUILD (полный доступ)" : "PLAN (только чтение)"));
        return;
    }
    if (utf8_key == "\r" || utf8_key == "\n") {
        submit();
        return;
    }
    if (utf8_key == "\x7f" || utf8_key == "\b") {
        if (!input_.empty()) {
            while (!input_.empty() && ((unsigned char)input_.back() & 0xC0) == 0x80)
                input_.pop_back();
            if (!input_.empty()) input_.pop_back();
        }
        return;
    }
    if (!utf8_key.empty() && (unsigned char)utf8_key[0] < 0x20 && utf8_key != "\t") return;
    input_ += utf8_key;
}

void TuiApp::cmd_model(const std::string& arg) {
    if (arg.empty()) {
        push_line("sys", "модель: " + cfg_.model() + " · провайдер: " + cfg_.provider +
                             " · смена: /model <id>");
        return;
    }
    cfg_.models[cfg_.provider] = arg;
    cfg_.save();
    push_line("sys", "модель → " + arg);
}

void TuiApp::cmd_cwd(const std::string& arg) {
    if (arg.empty()) {
        push_line("sys", "папка проекта: " + ctx_.cwd.string() + " · смена: /cwd <путь>");
        return;
    }
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(arg), ec);
    if (ec || !fs::is_directory(p, ec)) {
        push_line("err", "нет такой папки: " + arg);
        return;
    }
    ctx_.cwd = p;
    push_line("sys", "папка → " + p.string());
}

void TuiApp::cmd_tools() {
    nlohmann::json schemas = tool_schemas(mode_);
    std::string out;
    for (auto& t : schemas) {
        if (!out.empty()) out += " ";
        out += t["function"].value("name", "?");
    }
    push_line("sys", std::string("тулзы (") +
                         (mode_ == AgentMode::Build ? "BUILD" : "PLAN") + "): " + out);
}

void TuiApp::submit() {
    string text = trim(input_);
    input_.clear();
    if (text.empty()) return;

    // ---- slash commands ----
    if (text[0] == '/') {
        std::string cmd = text, arg;
        size_t sp = cmd.find(' ');
        if (sp != std::string::npos) {
            arg = trim(cmd.substr(sp + 1));
            cmd = cmd.substr(0, sp);
        }

        if (cmd == "/exit" || cmd == "/quit" || cmd == "/q") {
            quit_ = true;
            return;
        }
        if (cmd == "/help" || cmd == "/?") {
            push_line("sys", "команды: /help /clear /mode /build /plan /model [id] "
                             "/cwd [путь] /tools /key /exit");
            push_line("sys", "Tab — переключить build/plan · Ctrl+L — очистить экран");
            return;
        }
        if (busy_) {
            push_line("err", "дождись ответа агента (Ctrl+C — отмена)");
            return;
        }
        if (cmd == "/clear") {
            std::lock_guard<std::mutex> lock(mu_);
            lines_.clear();
            history_.clear();
            stream_buf_.clear();
            return;
        }
        if (cmd == "/mode") {
            handle_key("\t", false);
            return;
        }
        if (cmd == "/build") {
            mode_ = AgentMode::Build;
            ctx_.mode = mode_;
            push_line("sys", "режим → BUILD (полный доступ)");
            return;
        }
        if (cmd == "/plan") {
            mode_ = AgentMode::Plan;
            ctx_.mode = mode_;
            push_line("sys", "режим → PLAN (только чтение)");
            return;
        }
        if (cmd == "/model") {
            cmd_model(arg);
            return;
        }
        if (cmd == "/cwd" || cmd == "/cd") {
            cmd_cwd(arg);
            return;
        }
        if (cmd == "/tools") {
            cmd_tools();
            return;
        }
        if (cmd == "/key") {
            std::string k = cfg_.api_key();
            std::string tail = k.size() > 4 ? k.substr(k.size() - 4) : k;
            push_line("sys", k.empty() ? "ключа нет — выполни: screenmate auth login"
                                       : "ключ для " + cfg_.provider + " сохранён (••••" + tail + ")");
            return;
        }
        push_line("err", "неизвестная команда: " + cmd + " — попробуй /help");
        return;
    }

    if (busy_) return;
    push_line("user", text);
    run_agent_async(text);
}

void TuiApp::run_agent_async(string text) {
    busy_ = true;
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        stream_buf_.clear();
    }
    set_status("думаю…");
    std::thread([this, text = std::move(text)]() mutable {
        AgentCallbacks cb;
        cb.on_text = [this](const string& d) {
            std::lock_guard<std::mutex> lock(mu_);
            stream_buf_ += d;
        };
        cb.on_status = [this](const string& s) {
            // flush streamed step text into the transcript on each tool step
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (!stream_buf_.empty()) {
                    for (auto& l : split_lines(stream_buf_)) lines_.push_back({"ai", l});
                    stream_buf_.clear();
                }
            }
            set_status(s);
        };
        try {
            string answer = agent_turn(cfg_, ctx_, history_, text, cb,
                                       mode_ == AgentMode::Build ? "build" : "plan");
            {
                std::lock_guard<std::mutex> lock(mu_);
                stream_buf_.clear();
            }
            push_line("ai", answer);
            set_status("");
        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                stream_buf_.clear();
            }
            push_line("err", string("ошибка: ") + e.what());
            set_status("");
        }
        busy_ = false;
    }).detach();
}

int TuiApp::run() {
    setup_terminal();
    draw();

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
                if (ctrl && (wch == L'c' || wch == L'C')) { handle_key("c", true); continue; }
                if (ctrl && (wch == L'u' || wch == L'U')) { handle_key("u", true); continue; }
                if (ctrl && (wch == L'l' || wch == L'L')) { handle_key("l", true); continue; }
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
                if (n >= 3 && (buf[1] == '[' || buf[1] == 'O')) continue;
                continue;
            }
            if (buf[0] == '\t') { handle_key("\t", false); continue; }
            if (buf[0] == '\r' || buf[0] == '\n') { handle_key("\r", false); continue; }
            if (buf[0] == 127 || buf[0] == 8) { handle_key("\x7f", false); continue; }
            if (buf[0] == 3) { handle_key("c", true); continue; }
            if (buf[0] == 21) { handle_key("u", true); continue; }
            if (buf[0] == 12) { handle_key("l", true); continue; }
            if (buf[0] < 32) continue;
            handle_key(std::string((char*)buf, (size_t)n), false);
#endif
        }
    });

    while (!quit_) {
        draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    set_status("выход");
    draw();
    if (input_th.joinable()) input_th.detach();
    restore_terminal();
    return 0;
}

}  // namespace sm

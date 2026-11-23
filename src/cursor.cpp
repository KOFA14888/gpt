// Mouse / keyboard control (Windows SendInput / SetCursorPos). Stubs elsewhere.
//
// Design notes:
// - After a click the real cursor STAYS on the target so hover states and
//   follow-up drags work (the legacy Qt overlay restored the pointer; we
//   drive the real mouse instead).
// - MOVE settles briefly so apps receive WM_MOUSEMOVE before a click.
// - SCROLL anchors at (x,y) first — wheel goes to the window under the pointer.

#include "cursor.hpp"

#include "common.hpp"

#include <cctype>
#include <cstdio>
#include <vector>

#ifdef _WIN32

namespace sm {

void cursor_position(int* x, int* y) {
    POINT p{};
    GetCursorPos(&p);
    if (x) *x = p.x;
    if (y) *y = p.y;
}

static void move_to(int x, int y) {
    SetCursorPos(x, y);
}

static void sleep_ms(int ms) {
    if (ms > 0) Sleep((DWORD)ms);
}

static void mouse_down(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(INPUT));
}

static void mouse_up(DWORD flags) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(INPUT));
}

static void click_pair(DWORD down, DWORD up, int gap_ms = 40) {
    mouse_down(down);
    sleep_ms(gap_ms);
    mouse_up(up);
}

static void wheel(int notches) {
    // WHEEL_DELTA = 120 per notch; positive = scroll up (away).
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = (DWORD)(notches * WHEEL_DELTA);
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

std::string type_text(const std::string& utf8) {
    if (utf8.empty()) return "nothing to type";
    std::wstring w = utf8_to_wide(utf8);
    std::vector<INPUT> inputs;
    inputs.reserve(w.size() * 2);
    for (wchar_t ch : w) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = ch;
        in.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(in);
        INPUT up = in;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
    return "typed " + std::to_string(utf8.size()) + " bytes";
}

// ---- named keys / chords ----------------------------------------------------

struct KeyDef {
    const char* name;
    WORD vk;
};

static const KeyDef kKeys[] = {
    {"enter", VK_RETURN},     {"return", VK_RETURN},  {"esc", VK_ESCAPE},
    {"escape", VK_ESCAPE},    {"tab", VK_TAB},        {"space", VK_SPACE},
    {"backspace", VK_BACK},   {"delete", VK_DELETE},  {"del", VK_DELETE},
    {"up", VK_UP},            {"down", VK_DOWN},      {"left", VK_LEFT},
    {"right", VK_RIGHT},      {"home", VK_HOME},      {"end", VK_END},
    {"pageup", VK_PRIOR},     {"pagedown", VK_NEXT},  {"insert", VK_INSERT},
    {"f1", VK_F1},            {"f2", VK_F2},          {"f3", VK_F3},
    {"f4", VK_F4},            {"f5", VK_F5},          {"f6", VK_F6},
    {"f7", VK_F7},            {"f8", VK_F8},          {"f9", VK_F9},
    {"f10", VK_F10},          {"f11", VK_F11},        {"f12", VK_F12},
    {"ctrl", VK_CONTROL},     {"control", VK_CONTROL},{"alt", VK_MENU},
    {"shift", VK_SHIFT},      {"win", VK_LWIN},       {"meta", VK_LWIN},
};

static WORD vk_for(const std::string& tok) {
    for (auto& k : kKeys) {
        if (tok == k.name) return k.vk;
    }
    // single printable character → virtual key via VkKeyScanW
    if (tok.size() == 1) {
        SHORT v = VkKeyScanW(tok[0]);
        if (v != -1) return (WORD)(v & 0xFF);
    }
    return 0;
}

static void key_tap(WORD vk) {
    INPUT down{};
    down.type = INPUT_KEYBOARD;
    down.ki.wVk = vk;
    SendInput(1, &down, sizeof(INPUT));
    sleep_ms(15);
    INPUT up = down;
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &up, sizeof(INPUT));
}

std::string press_key(const std::string& name_in) {
    std::string name = to_lower(name_in);
    // strip spaces: "ctrl + s" -> "ctrl+s"
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    if (name.empty()) return "empty key";

    std::vector<std::string> parts;
    std::string cur;
    for (char c : name) {
        if (c == '+' || c == '-') {
            if (!cur.empty()) parts.push_back(cur), cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return "empty key";

    std::vector<WORD> mods;
    WORD main_vk = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        WORD vk = vk_for(parts[i]);
        if (!vk) return "unknown key: " + parts[i];
        bool is_last = (i + 1 == parts.size());
        bool is_mod = parts[i] == "ctrl" || parts[i] == "control" ||
                      parts[i] == "alt" || parts[i] == "shift" ||
                      parts[i] == "win" || parts[i] == "meta";
        if (is_last && !is_mod) main_vk = vk;
        else mods.push_back(vk);
    }
    if (!main_vk && !mods.empty()) {
        // chord of only modifiers — tap them as a unit (rare)
        main_vk = mods.back();
        mods.pop_back();
    }
    if (!main_vk) return "no main key in " + name;

    for (WORD m : mods) {
        INPUT d{};
        d.type = INPUT_KEYBOARD;
        d.ki.wVk = m;
        SendInput(1, &d, sizeof(INPUT));
        sleep_ms(10);
    }
    key_tap(main_vk);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) {
        INPUT u{};
        u.type = INPUT_KEYBOARD;
        u.ki.wVk = *it;
        u.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &u, sizeof(INPUT));
        sleep_ms(5);
    }
    return "pressed " + name;
}

// ---- actions ----------------------------------------------------------------

static std::string pos_suffix() {
    int x = 0, y = 0;
    cursor_position(&x, &y);
    return " @ " + std::to_string(x) + "," + std::to_string(y);
}

std::string cursor_perform(const CursorAction& a) {
    std::string kind = a.kind;
    for (auto& c : kind) c = (char)std::toupper((unsigned char)c);

    if (kind == "POS") {
        int x = 0, y = 0;
        cursor_position(&x, &y);
        return "cursor at " + std::to_string(x) + "," + std::to_string(y);
    }

    if (kind == "MOVE" || kind == "HOVER") {
        move_to(a.x, a.y);
        // settle so the target window gets WM_MOUSEMOVE / hover highlight
        sleep_ms(kind == "HOVER" ? 180 : 40);
        return (kind == "HOVER" ? "hovered " : "moved to ") +
               std::to_string(a.x) + "," + std::to_string(a.y) + pos_suffix();
    }

    if (kind == "CLICK" || kind == "RIGHT_CLICK" || kind == "MIDDLE_CLICK" ||
        kind == "DOUBLE_CLICK") {
        move_to(a.x, a.y);
        sleep_ms(50);  // hover settle before press
        if (kind == "CLICK")
            click_pair(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        else if (kind == "RIGHT_CLICK")
            click_pair(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
        else if (kind == "MIDDLE_CLICK")
            click_pair(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP);
        else {  // DOUBLE_CLICK
            click_pair(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 40);
            sleep_ms(50);
            click_pair(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 40);
        }
        sleep_ms(30);
        return kind + " at " + std::to_string(a.x) + "," + std::to_string(a.y) +
               pos_suffix();
    }

    if (kind == "DRAG") {
        move_to(a.x, a.y);
        sleep_ms(60);
        mouse_down(MOUSEEVENTF_LEFTDOWN);
        sleep_ms(40);
        // intermediate steps so the target sees a real drag, not a teleport
        const int steps = 8;
        int x0 = a.x, y0 = a.y, x1 = a.x2, y1 = a.y2;
        for (int i = 1; i <= steps; ++i) {
            int nx = x0 + (x1 - x0) * i / steps;
            int ny = y0 + (y1 - y0) * i / steps;
            move_to(nx, ny);
            sleep_ms(12);
        }
        sleep_ms(40);
        mouse_up(MOUSEEVENTF_LEFTUP);
        sleep_ms(30);
        return "dragged " + std::to_string(x0) + "," + std::to_string(y0) +
               " → " + std::to_string(x1) + "," + std::to_string(y1) +
               pos_suffix();
    }

    if (kind == "SCROLL") {
        int notches = a.dx != 0 ? a.dx : a.dy;
        if (notches == 0) notches = 1;
        if (a.x != 0 || a.y != 0) {
            move_to(a.x, a.y);
            sleep_ms(40);
        }
        // send in small batches so apps handle it smoothly
        int sign = notches < 0 ? -1 : 1;
        int left = notches < 0 ? -notches : notches;
        while (left > 0) {
            int batch = left > 5 ? 5 : left;
            wheel(sign * batch);
            left -= batch;
            sleep_ms(25);
        }
        return "scrolled " + std::to_string(notches) + " notches" +
               pos_suffix();
    }

    if (kind == "KEY") {
        if (a.text.empty())
            return "KEY requires text (e.g. \"enter\", \"ctrl+s\", \"esc\")";
        return press_key(a.text);
    }

    if (kind == "TYPE") {
        if (a.text.empty()) return "TYPE requires text";
        return type_text(a.text);
    }

    return "unknown action " + a.kind +
           " (use MOVE|HOVER|CLICK|RIGHT_CLICK|MIDDLE_CLICK|DOUBLE_CLICK|"
           "DRAG|SCROLL|KEY|TYPE|POS)";
}

}  // namespace sm

#else  // !_WIN32

namespace sm {

void cursor_position(int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
}
std::string type_text(const std::string&) { return "typing is Windows-only"; }
std::string press_key(const std::string&) { return "keys are Windows-only"; }
std::string cursor_perform(const CursorAction& a) {
    return "cursor control is Windows-only (action " + a.kind + ")";
}

}  // namespace sm

#endif

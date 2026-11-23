// Mouse / keyboard control (Windows SendInput). Stubs elsewhere.

#include "cursor.hpp"

#include "common.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

static void click_button(DWORD down, DWORD up) {
    mouse_event(down, 0, 0, 0, 0);
    Sleep(30);
    mouse_event(up, 0, 0, 0, 0);
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

std::string cursor_perform(const CursorAction& a) {
    if (a.kind == "MOVE") {
        move_to(a.x, a.y);
        return "moved to " + std::to_string(a.x) + "," + std::to_string(a.y);
    }
    if (a.kind == "CLICK") {
        move_to(a.x, a.y);
        Sleep(40);
        click_button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        return "left click at " + std::to_string(a.x) + "," + std::to_string(a.y);
    }
    if (a.kind == "RIGHT_CLICK") {
        move_to(a.x, a.y);
        Sleep(40);
        click_button(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
        return "right click at " + std::to_string(a.x) + "," + std::to_string(a.y);
    }
    if (a.kind == "DOUBLE_CLICK") {
        move_to(a.x, a.y);
        Sleep(40);
        click_button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        Sleep(60);
        click_button(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        return "double click at " + std::to_string(a.x) + "," + std::to_string(a.y);
    }
    if (a.kind == "TYPE") return type_text(a.text);
    return "unknown action " + a.kind;
}

}  // namespace sm

#else

namespace sm {
void cursor_position(int* x, int* y) {
    if (x) *x = 0;
    if (y) *y = 0;
}
std::string type_text(const std::string&) { return "typing is Windows-only"; }
std::string cursor_perform(const CursorAction& a) {
    return "cursor control is Windows-only (action " + a.kind + ")";
}
}  // namespace sm

#endif

#pragma once
// Real mouse / keyboard control for the AI (Windows SendInput / SetCursorPos).
// Stubs on other platforms so CI smoke builds still link.

#include <string>

namespace sm {

struct CursorAction {
    // MOVE | HOVER | CLICK | RIGHT_CLICK | MIDDLE_CLICK | DOUBLE_CLICK |
    // DRAG | SCROLL | KEY | TYPE | POS
    std::string kind;
    int x = 0;
    int y = 0;    // primary target (MOVE/CLICK/DRAG start/SCROLL anchor)
    int x2 = 0;   // DRAG end
    int y2 = 0;
    int dx = 0;   // SCROLL wheel notches (positive = up / away)
    int dy = 0;   // reserved
    std::string text;  // TYPE payload, or KEY name (e.g. "enter", "ctrl+s")
};

// Perform one action. Returns human-readable result (includes final pos).
std::string cursor_perform(const CursorAction& a);

// Current cursor position.
void cursor_position(int* x, int* y);

// Type UTF-8 text via Unicode SendInput (Windows) — no-op elsewhere.
std::string type_text(const std::string& utf8);

// Send a named key / chord ("enter", "esc", "ctrl+s", "alt+tab", "f5").
std::string press_key(const std::string& name);

}  // namespace sm

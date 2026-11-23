#pragma once
// Virtual cursor control (AI moves the real mouse on Windows).

#include <string>

namespace sm {

struct CursorAction {
    std::string kind;  // MOVE | CLICK | RIGHT_CLICK | DOUBLE_CLICK | TYPE
    int x = 0;
    int y = 0;
    std::string text;  // for TYPE
};

// Perform one action. Returns human-readable result.
std::string cursor_perform(const CursorAction& a);

// Current cursor position.
void cursor_position(int* x, int* y);

// Type UTF-8 text via Unicode SendInput (Windows) — no-op elsewhere.
std::string type_text(const std::string& utf8);

}  // namespace sm

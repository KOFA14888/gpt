#pragma once
// Native desktop window (Win32 + DirectX 11 + Dear ImGui). No browser.

namespace sm {

// Run the desktop chat app. Blocks until the window closes.
// Returns process exit code. On non-Windows returns -1 (caller falls back).
int desktop_run();

}  // namespace sm

#pragma once
// Graphical chat UI: embedded HTML/CSS/JS on localhost, opens a browser window.

namespace sm {

// Start GUI HTTP server and open the system browser. Blocks until interrupt.
// Returns process exit code.
int gui_run(int port);

}  // namespace sm

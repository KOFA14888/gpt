#pragma once
// Screen understanding: screenshot (JPEG bytes) + visible window list.

#include <cstdint>
#include <string>
#include <vector>

namespace sm {

struct WindowInfo {
    std::string title;
    std::string process;
    bool visible = false;
};

// Capture virtual screen (or configured region) as JPEG.
// Returns empty on failure / unsupported OS.
std::string capture_screen_jpeg(int max_side = 1600, int quality = 70,
                                const std::vector<int>& region = {});

// Enumerate top-level visible windows with titles + process names.
std::vector<WindowInfo> list_windows();

// Encode raw RGB (3 bpp) to JPEG via stb. Used by capture impl.
std::string encode_jpeg(const uint8_t* rgb, int w, int h, int quality);

}  // namespace sm

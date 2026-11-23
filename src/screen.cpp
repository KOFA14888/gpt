// Screen capture + window enumeration (Windows GDI / Win32).
// Non-Windows: stubs so the project still builds for CI smoke tests.

#include "screen.hpp"

#include "common.hpp"

#include <algorithm>
#include <vector>

#ifdef _WIN32
// shellapi.h / psapi.h come from common.hpp (global scope)
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace sm {

std::string encode_jpeg(const uint8_t* rgb, int w, int h, int quality) {
    std::string buf;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size);
        },
        &buf, w, h, 3, rgb, quality);
    return buf;
}

static bool grab_region(int x, int y, int w, int h, std::vector<uint8_t>& rgb, int* ow, int* oh) {
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!mem || !bmp) {
        if (mem) DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    SelectObject(mem, bmp);
    if (!BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY)) {
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;  // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    std::vector<uint8_t> bits((size_t)w * h * 4);
    if (!GetDIBits(mem, bmp, 0, (UINT)h, bits.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    rgb.resize((size_t)w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = bits[i * 4 + 2];
        rgb[i * 3 + 1] = bits[i * 4 + 1];
        rgb[i * 3 + 2] = bits[i * 4 + 0];
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    *ow = w;
    *oh = h;
    return true;
}

std::string capture_screen_jpeg(int max_side, int quality, const std::vector<int>& region) {
    int x = 0, y = 0, w = GetSystemMetrics(SM_CXVIRTUALSCREEN),
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int ox = GetSystemMetrics(SM_XVIRTUALSCREEN), oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    x = ox;
    y = oy;
    if (region.size() == 4 && region[2] > 0 && region[3] > 0) {
        x = region[0];
        y = region[1];
        w = region[2];
        h = region[3];
    }
    std::vector<uint8_t> rgb;
    int fw = 0, fh = 0;
    if (!grab_region(x, y, w, h, rgb, &fw, &fh)) return "";

    // downscale by simple box filter if needed
    int side = std::max(fw, fh);
    if (max_side > 0 && side > max_side) {
        double scale = (double)max_side / side;
        int nw = std::max(1, (int)(fw * scale));
        int nh = std::max(1, (int)(fh * scale));
        std::vector<uint8_t> small((size_t)nw * nh * 3);
        for (int yy = 0; yy < nh; ++yy) {
            int sy0 = yy * fh / nh, sy1 = std::max(sy0 + 1, (yy + 1) * fh / nh);
            for (int xx = 0; xx < nw; ++xx) {
                int sx0 = xx * fw / nw, sx1 = std::max(sx0 + 1, (xx + 1) * fw / nw);
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int sy = sy0; sy < sy1; ++sy)
                    for (int sx = sx0; sx < sx1; ++sx) {
                        size_t i = ((size_t)sy * fw + sx) * 3;
                        r += rgb[i];
                        g += rgb[i + 1];
                        b += rgb[i + 2];
                        ++n;
                    }
                size_t o = ((size_t)yy * nw + xx) * 3;
                small[o] = (uint8_t)(r / n);
                small[o + 1] = (uint8_t)(g / n);
                small[o + 2] = (uint8_t)(b / n);
            }
        }
        rgb.swap(small);
        fw = nw;
        fh = nh;
    }
    return encode_jpeg(rgb.data(), fw, fh, quality);
}

struct EnumCtx {
    std::vector<WindowInfo>* out;
};

static std::string process_name_of(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return "";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "";
    wchar_t buf[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    std::string name = "";
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        name = wide_to_utf8(std::wstring(buf, n));
        auto pos = name.find_last_of("\\/");
        if (pos != std::string::npos) name = name.substr(pos + 1);
    }
    CloseHandle(h);
    return name;
}

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring wtitle(len, 0);
    GetWindowTextW(hwnd, wtitle.data(), len + 1);
    if (wtitle.empty()) return TRUE;
    // skip tool windows / zero-size
    RECT rc;
    if (!GetWindowRect(hwnd, &rc) || (rc.right - rc.left) < 50 || (rc.bottom - rc.top) < 50)
        return TRUE;
    WindowInfo wi;
    wi.title = wide_to_utf8(wtitle);
    wi.process = process_name_of(hwnd);
    wi.visible = true;
    ctx->out->push_back(std::move(wi));
    return TRUE;
}

std::vector<WindowInfo> list_windows() {
    std::vector<WindowInfo> out;
    EnumCtx ctx{&out};
    EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

}  // namespace sm

#else  // !_WIN32

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace sm {

std::string encode_jpeg(const uint8_t* rgb, int w, int h, int quality) {
    std::string buf;
    stbi_write_jpg_to_func(
        [](void* ctx, void* data, int size) {
            static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size);
        },
        &buf, w, h, 3, rgb, quality);
    return buf;
}

std::string capture_screen_jpeg(int, int, const std::vector<int>&) {
    return "";  // screenshot is Windows-only in this build
}

std::vector<WindowInfo> list_windows() {
    return {};
}

}  // namespace sm

#endif

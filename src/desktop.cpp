// Native desktop chat: Win32 window + DirectX 11 + Dear ImGui.
// One .exe, no browser. Non-Windows builds get a stub (CI smoke).

#include "desktop.hpp"

#include "agent.hpp"
#include "common.hpp"
#include "config.hpp"
#include "tools.hpp"

#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi.h>
#include <tchar.h>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "misc/cpp/imgui_stdlib.h"

// Header intentionally hides this behind #if 0 — copy the declaration here.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace sm {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// App state (shared with agent worker thread)
// ---------------------------------------------------------------------------

struct ChatMsg {
    int role = 0;  // 0 user, 1 ai, 2 sys, 3 err
    std::string text;
    float born = 0.f;  // ImGui time for fade-in
};

struct AppState {
    Config cfg;
    ToolContext ctx;
    std::vector<json> history;
    std::vector<ChatMsg> msgs;
    std::mutex mu;
    std::string input;
    std::string status;
    std::string stream_buf;   // growing AI answer while streaming
    bool streaming = false;
    bool busy = false;
    bool need_auth = false;
    bool want_clear = false;
    bool plan_mode = false;
    bool focus_input = false;
    int scroll_to_end = 0;
    std::atomic<bool> quit{false};
};

static AppState* g_app = nullptr;

static void push_msg(AppState& a, int role, const std::string& text) {
    std::lock_guard<std::mutex> lock(a.mu);
    ChatMsg m;
    m.role = role;
    m.text = text;
    m.born = 0.f;  // 0 = already visible (no fade)
    a.msgs.push_back(std::move(m));
    a.scroll_to_end = 2;
}

static void start_agent(AppState& a, std::string text) {
    if (a.busy) return;
    a.busy = true;
    a.streaming = true;
    a.stream_buf.clear();
    {
        std::lock_guard<std::mutex> lock(a.mu);
        ChatMsg m;
        m.role = 0;
        m.text = text;
        m.born = 0.f;
        a.msgs.push_back(m);
        a.status = "думаю…";
        a.scroll_to_end = 2;
    }
    std::thread([&a, text = std::move(text)]() mutable {
        AgentCallbacks cb;
        cb.on_text = [&a](const std::string& d) {
            std::lock_guard<std::mutex> lock(a.mu);
            a.stream_buf += d;
            a.scroll_to_end = 1;
        };
        cb.on_status = [&a](const std::string& s) {
            std::lock_guard<std::mutex> lock(a.mu);
            a.status = s;
        };
        std::string reply, err;
        try {
            reply = agent_turn(a.cfg, a.ctx, a.history, text, cb,
                               a.plan_mode ? "plan" : "build");
        } catch (const std::exception& e) {
            err = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(a.mu);
            ChatMsg m;
            if (err.empty()) {
                m.role = 1;
                m.text = reply.empty() ? a.stream_buf : reply;
            } else {
                m.role = 3;
                m.text = err;
            }
            m.born = 0.f;
            a.msgs.push_back(std::move(m));
            a.stream_buf.clear();
            a.status.clear();
            a.streaming = false;
            a.busy = false;
            a.scroll_to_end = 2;
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// Style — dark modern accent
// ---------------------------------------------------------------------------

static void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10.f;
    s.ChildRounding = 12.f;
    s.FrameRounding = 8.f;
    s.PopupRounding = 8.f;
    s.ScrollbarRounding = 8.f;
    s.GrabRounding = 6.f;
    s.TabRounding = 8.f;
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(12, 10);
    s.ItemSpacing = ImVec2(10, 8);
    s.ScrollbarSize = 12.f;
    s.WindowBorderSize = 0.f;
    s.ChildBorderSize = 0.f;
    s.FrameBorderSize = 0.f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.063f, 0.082f, 1.f);
    c[ImGuiCol_ChildBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.18f, 0.20f, 0.26f, 0.6f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.11f, 0.15f, 1.f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.15f, 0.20f, 1.f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.17f, 0.24f, 1.f);
    c[ImGuiCol_Button] = ImVec4(0.36f, 0.34f, 0.95f, 1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.42f, 1.f, 1.f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.28f, 0.85f, 1.f);
    c[ImGuiCol_Header] = ImVec4(0.30f, 0.32f, 0.75f, 0.65f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.38f, 0.90f, 0.75f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.30f, 0.80f, 0.90f);
    c[ImGuiCol_Text] = ImVec4(0.91f, 0.93f, 0.96f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.53f, 0.60f, 1.f);
    c[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.85f, 0.75f, 1.f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.48f, 0.46f, 0.98f, 1.f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.58f, 0.55f, 1.f, 1.f);
    c[ImGuiCol_Separator] = ImVec4(0.18f, 0.20f, 0.26f, 1.f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.36f, 0.34f, 0.95f, 0.4f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.45f, 0.42f, 1.f, 0.6f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.055f, 0.07f, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.30f, 0.38f, 1.f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.38f, 0.48f, 1.f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.42f, 0.95f, 1.f);
    c[ImGuiCol_NavCursor] = c[ImGuiCol_Header];
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static void bubble(AppState& a, const ChatMsg& m, float now) {
    float fade = 1.f;
    if (m.born > 0.f) {
        float age = now - m.born;
        fade = age < 0.25f ? age / 0.25f : 1.f;
    }
    float rise = (1.f - fade) * 12.f;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.f);

    const char* label = "";
    ImVec4 tint(1, 1, 1, 1);
    bool wrap = true;
    float max_w = 0.f;

    if (m.role == 0) {
        label = "##u";
        tint = ImVec4(0.20f, 0.22f, 0.34f, 1.f);
        max_w = ImGui::GetContentRegionAvail().x * 0.78f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             ImGui::GetContentRegionAvail().x * 0.22f);
    } else if (m.role == 1) {
        label = "##a";
        tint = ImVec4(0.09f, 0.10f, 0.14f, 1.f);
        max_w = ImGui::GetContentRegionAvail().x * 0.92f;
    } else if (m.role == 2) {
        label = "##s";
        tint = ImVec4(0.f, 0.f, 0.f, 0.f);
    } else {
        label = "##e";
        tint = ImVec4(0.22f, 0.10f, 0.14f, 1.f);
    }

    if (m.role == 2) {
        ImGui::TextDisabled("%s", m.text.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, tint);
        ImGui::BeginChild(label, ImVec2(max_w, 0),
                          ImGuiWindowFlags_ChildWindow |
                              ImGuiWindowFlags_NoScrollbar |
                              (wrap ? 0 : 0));
        ImGui::TextWrapped("%s", m.text.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar(3);
    // visual rise offset applied via dummy after — simpler: skip rise if complex
    (void)rise;
}

static void draw_ui(AppState& a) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("ScreenMate", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);

    // ---- header ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.085f, 0.115f, 1.f));
    ImGui::BeginChild("header", ImVec2(0, 54),
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        float h = 54.f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetWindowPos();
        // accent bar
        dl->AddRectFilled(p, ImVec2(p.x + 4, p.y + h),
                          IM_COL32(110, 100, 255, 255));
        ImGui::SetCursorPosY(12.f);
        ImGui::SetCursorPosX(18.f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.50f, 1.f, 1.f));
        ImGui::TextUnformatted("◉");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted("ScreenMate AI");
        ImGui::SameLine(0, 10);
        ImGui::TextDisabled("· desktop");

        // mode toggle on the right
        float toggle_w = 190.f;
        ImGui::SameLine(ImGui::GetWindowWidth() - toggle_w - 18.f);
        bool plan = a.plan_mode;
        if (ImGui::BeginTable("mode", 2,
                              ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1);
            ImGui::TableSetupColumn("p", ImGuiTableColumnFlags_WidthStretch, 1);
            ImGui::TableNextColumn();
            if (!plan) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.36f, 0.34f, 0.95f, 1.f));
            }
            if (ImGui::Button("BUILD", ImVec2(-1, 30))) a.plan_mode = false;
            if (!plan) ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (plan) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.18f, 0.55f, 0.42f, 1.f));
            }
            if (ImGui::Button("PLAN", ImVec2(-1, 30))) a.plan_mode = true;
            if (plan) ImGui::PopStyleColor();
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- chat ----
    ImGui::SetCursorPosY(54.f + 6.f);
    ImGui::BeginChild("chat", ImVec2(0, -140.f),
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    {
        std::lock_guard<std::mutex> lock(a.mu);
        float now = (float)ImGui::GetTime();
        if (a.msgs.empty()) {
            ImGui::Dummy(ImVec2(0, 40));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.50f, 1.f, 1.f));
            ImGui::SetWindowFontScale(1.35f);
            ImGui::TextUnformatted("  Чем займёмся?");
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopStyleColor();
            ImGui::TextDisabled(
                "  Опиши задачу — агент прочитает код репозитория\n"
                "  и поможет.  Tab — сменить BUILD / PLAN.");
        }
        for (const auto& m : a.msgs) {
            bubble(a, m, now);
            ImGui::Dummy(ImVec2(0, 4));
        }
        if (a.streaming) {
            float t = (float)ImGui::GetTime();
            int dots = 1 + ((int)(t * 3.f) % 3);
            std::string s = "▍ ";
            for (int i = 0; i < dots; ++i) s += "●";
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.45f, 0.45f, 0.60f, 0.9f));
            ImGui::TextUnformatted(s.c_str());
            ImGui::PopStyleColor();
            if (!a.stream_buf.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.85f, 0.87f, 0.93f, 1.f));
                ImGui::TextWrapped("%s", a.stream_buf.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (a.scroll_to_end > 0) {
            ImGui::SetScrollHereY(1.0f);
            a.scroll_to_end--;
        }
    }
    ImGui::EndChild();

    // ---- status + input ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.075f, 0.085f, 0.115f, 1.f));
    ImGui::BeginChild("footer", ImVec2(0, 140),
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::SetCursorPos(ImVec2(14, 8));
        {
            std::lock_guard<std::mutex> lock(a.mu);
            if (!a.status.empty()) {
                float t = (float)ImGui::GetTime();
                float pulse = 0.65f + 0.35f * (0.5f + 0.5f * sinf(t * 4.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(0.30f * pulse + 0.2f,
                                             0.85f * pulse + 0.1f,
                                             0.75f * pulse + 0.1f, 1.f));
                ImGui::Text("●  %s", a.status.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextDisabled("Tab: build/plan   ·   Enter: отправить");
            }
        }

        ImGui::SetCursorPos(ImVec2(14, 34));
        ImGui::BeginChild("inputbox",
                          ImVec2(-90, -14),
                          ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemWidth(-8);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, 90));
        {
            std::lock_guard<std::mutex> lock(a.mu);
            if (ImGui::InputTextMultiline("##input", &a.input,
                                          ImVec2(0, 72),
                                          ImGuiInputTextFlags_AllowTabInput |
                                              ImGuiInputTextFlags_CtrlEnterForNewLine)) {
            }
        }
        ImGui::EndChild();

        // send button
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 76, 34));
        bool busy;
        {
            std::lock_guard<std::mutex> lock(a.mu);
            busy = a.busy;
        }
        if (busy) {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.34f, 0.95f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.45f, 0.42f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.28f, 0.26f, 0.80f, 1.f));
        ImGui::SetCursorPosY(34);
        if (ImGui::Button("Отправить", ImVec2(64, 72))) {
            std::string text;
            bool go = false;
            {
                std::lock_guard<std::mutex> lock(a.mu);
                // trim
                auto& s = a.input;
                size_t b = s.find_first_not_of(" \t\r\n");
                size_t e = s.find_last_not_of(" \t\r\n");
                if (b != std::string::npos) {
                    text = s.substr(b, e - b + 1);
                    s.clear();
                    go = true;
                }
            }
            if (go) start_agent(a, std::move(text));
        }
        ImGui::PopStyleColor(3);
        if (busy) ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// Win32 + D3D11
// ---------------------------------------------------------------------------

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                             &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int desktop_run() {
    AppState app;
    app.cfg = Config::load();
    app.ctx.cfg = &app.cfg;
    app.ctx.cwd = fs::current_path();
    app.need_auth = app.cfg.api_key().empty();
    g_app = &app;
    if (app.need_auth) {
        push_msg(app, 2, "Нет API-ключа — выполни screenmate auth login и перезапустите.");
        push_msg(app, 2, "Tab — BUILD/PLAN · Enter — отправить · Ctrl+Enter — перенос");
    } else {
        push_msg(app, 2, "ScreenMate AI — console coding agent");
        push_msg(app, 2, "Tab — BUILD/PLAN · Enter — отправить");
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = {sizeof(wc),
                      CS_CLASSDC,
                      WndProc,
                      0L,
                      0L,
                      GetModuleHandle(nullptr),
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      L"ScreenMateAI",
                      nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"ScreenMate AI",
                              WS_OVERLAPPEDWINDOW, 100, 100, 1100, 760,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_style();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // global shortcuts
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
            std::lock_guard<std::mutex> lock(app.mu);
            app.plan_mode = !app.plan_mode;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            // leave input first; second Esc quits if empty
            bool empty;
            {
                std::lock_guard<std::mutex> lock(app.mu);
                empty = app.input.empty();
            }
            if (empty) PostMessage(hwnd, WM_CLOSE, 0, 0);
        }

        draw_ui(app);

        ImGui::Render();
        const float clear_color[4] = {0.055f, 0.063f, 0.082f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                                nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                   clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_app = nullptr;
    return 0;
}

}  // namespace sm

#else  // !_WIN32

namespace sm {
int desktop_run() { return -1; }
}  // namespace sm

#endif

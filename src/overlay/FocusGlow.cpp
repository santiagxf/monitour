#include "FocusGlow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace monitour::overlay {

namespace {

constexpr wchar_t kClassName[]      = L"MonitourFocusGlow";
constexpr UINT    WM_FOCUSGLOW_FLASH = WM_APP + 1;
constexpr UINT_PTR kTimerId         = 1;
constexpr UINT    kFrameMs          = 16;   // ~60 Hz
constexpr ULONGLONG kPulseMs        = 350;  // one pulse
constexpr ULONGLONG kTotalMs        = kPulseMs;  // single pulse
constexpr int     kBorderThickness  = 22;   // px from outer edge

// Corner color #FF40C0 (magenta) at the four corners; midpoint color #8040FF
// (violet) at the midpoint of each edge. Bilinear blend in between.
struct ColorF { float r, g, b; };
constexpr ColorF kCorner = {1.0f, 0x40 / 255.0f, 0xC0 / 255.0f};
constexpr ColorF kMid    = {0x80 / 255.0f, 0x40 / 255.0f, 1.0f};

BOOL CALLBACK enumProc(HMONITOR hmon, HDC, LPRECT rc, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<std::pair<HMONITOR, RECT>>*>(lp);
    out->emplace_back(hmon, *rc);
    return TRUE;
}

}  // namespace

struct FocusGlow::Window {
    HWND      hwnd{nullptr};
    HMONITOR  monitor{nullptr};
    RECT      rect{};
    HDC       memDc{nullptr};
    HBITMAP   bmp{nullptr};
    HBITMAP   oldBmp{nullptr};
    void*     bits{nullptr};
    int       width{0};
    int       height{0};
    ULONGLONG startMs{0};
    bool      active{false};
};

FocusGlow::~FocusGlow() { destroy(); }

bool FocusGlow::create(HINSTANCE hinst) {
    hinst_ = hinst;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &FocusGlow::wndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // ignore "already registered" on re-create

    rebuildWindows();
    return !windows_.empty();
}

void FocusGlow::destroy() {
    teardownWindows();
    hinst_ = nullptr;
}

void FocusGlow::onDisplayChange() {
    teardownWindows();
    rebuildWindows();
}

void FocusGlow::flash(HMONITOR monitor) {
    HWND target = nullptr;
    {
        std::lock_guard lock{mutex_};
        for (auto* w : windows_) {
            if (w->monitor == monitor) { target = w->hwnd; break; }
        }
    }
    if (target) PostMessageW(target, WM_FOCUSGLOW_FLASH, 0, 0);
}

void FocusGlow::rebuildWindows() {
    if (!hinst_) return;

    // Per-Monitor V2 for the duration of monitor enumeration + window creation.
    // The rest of the app runs DPI-unaware; without this, on a fractionally
    // scaled monitor (e.g. 125%) Windows virtualises both the rcMonitor we
    // get back and the rect we pass to CreateWindowExW, and the two round
    // differently — leaving a 1-px gap on the right/bottom edge of the glow
    // window. Switching the thread context aligns the two coordinate spaces.
    auto prevCtx = SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::vector<std::pair<HMONITOR, RECT>> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &enumProc,
                        reinterpret_cast<LPARAM>(&monitors));

    std::lock_guard lock{mutex_};
    for (auto& [hmon, rc] : monitors) {
        auto* w = new Window();
        w->monitor = hmon;
        w->rect    = rc;
        w->width   = rc.right - rc.left;
        w->height  = rc.bottom - rc.top;
        if (w->width <= 0 || w->height <= 0) { delete w; continue; }

        w->hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kClassName, L"", WS_POPUP,
            rc.left, rc.top, w->width, w->height,
            nullptr, nullptr, hinst_, w);
        if (!w->hwnd) { delete w; continue; }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth       = w->width;
        bi.bmiHeader.biHeight      = -w->height;  // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screenDc = GetDC(nullptr);
        w->bmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &w->bits,
                                  nullptr, 0);
        w->memDc = CreateCompatibleDC(screenDc);
        ReleaseDC(nullptr, screenDc);

        if (!w->bmp || !w->memDc || !w->bits) {
            if (w->memDc) DeleteDC(w->memDc);
            if (w->bmp) DeleteObject(w->bmp);
            DestroyWindow(w->hwnd);
            delete w;
            continue;
        }
        w->oldBmp = static_cast<HBITMAP>(SelectObject(w->memDc, w->bmp));
        renderBitmap(*w);
        windows_.push_back(w);
    }

    if (prevCtx) SetThreadDpiAwarenessContext(prevCtx);
}

void FocusGlow::teardownWindows() {
    std::lock_guard lock{mutex_};
    for (auto* w : windows_) {
        if (w->hwnd) KillTimer(w->hwnd, kTimerId);
        if (w->memDc) {
            if (w->oldBmp) SelectObject(w->memDc, w->oldBmp);
            DeleteDC(w->memDc);
        }
        if (w->bmp)  DeleteObject(w->bmp);
        if (w->hwnd) DestroyWindow(w->hwnd);
        delete w;
    }
    windows_.clear();
}

void FocusGlow::renderBitmap(Window& w) {
    auto*       px    = static_cast<uint8_t*>(w.bits);
    const int   W     = w.width;
    const int   H     = w.height;
    const float th    = static_cast<float>(kBorderThickness);
    // Gaussian sigma chosen so the glow has nearly faded by the inner edge.
    const float sigma = th * 0.45f;

    for (int y = 0; y < H; ++y) {
        const float ny = (H > 1) ? static_cast<float>(y) / (H - 1) : 0.5f;
        for (int x = 0; x < W; ++x) {
            const float nx = (W > 1) ? static_cast<float>(x) / (W - 1) : 0.5f;

            // Distance from the nearest outer edge — 0 at the edge, grows
            // inward. Pixels deeper than the border thickness are transparent.
            const int dxI = std::min(x, W - 1 - x);
            const int dyI = std::min(y, H - 1 - y);
            const int dI  = std::min(dxI, dyI);

            float alpha = 0.0f;
            if (dI < kBorderThickness) {
                const float u = static_cast<float>(dI) / sigma;
                alpha = std::exp(-0.5f * u * u);
            }

            // Pick the parametric position along the closer edge so the
            // corner→midpoint→corner blend follows the rim.
            float t;
            if (dxI < dyI) {
                // closer to a vertical edge: blend along y
                t = std::abs(ny - 0.5f) * 2.0f;
            } else {
                // closer to a horizontal edge (or corner): blend along x
                t = std::abs(nx - 0.5f) * 2.0f;
            }
            t = std::clamp(t, 0.0f, 1.0f);  // 1 at corner, 0 at midpoint
            const float r = kCorner.r * t + kMid.r * (1.0f - t);
            const float g = kCorner.g * t + kMid.g * (1.0f - t);
            const float b = kCorner.b * t + kMid.b * (1.0f - t);

            // BGRA, premultiplied — required by UpdateLayeredWindow + AC_SRC_ALPHA.
            const uint8_t a8 = static_cast<uint8_t>(std::lround(alpha * 255.0f));
            const int     idx = (y * W + x) * 4;
            px[idx + 0] = static_cast<uint8_t>(std::lround(b * alpha * 255.0f));
            px[idx + 1] = static_cast<uint8_t>(std::lround(g * alpha * 255.0f));
            px[idx + 2] = static_cast<uint8_t>(std::lround(r * alpha * 255.0f));
            px[idx + 3] = a8;
        }
    }
}

LRESULT CALLBACK FocusGlow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_FOCUSGLOW_FLASH: {
            w->startMs = GetTickCount64();
            w->active  = true;

            // Show at zero opacity first so there's no single-frame full-alpha
            // pop before the animation ramps in.
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 0, AC_SRC_ALPHA};
            POINT srcPt{0, 0};
            SIZE  sz{w->width, w->height};
            POINT pos{w->rect.left, w->rect.top};
            UpdateLayeredWindow(hwnd, nullptr, &pos, &sz, w->memDc, &srcPt, 0,
                                &bf, ULW_ALPHA);

            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetTimer(hwnd, kTimerId, kFrameMs, nullptr);
            return 0;
        }
        case WM_TIMER: {
            if (wp != kTimerId || !w->active) return 0;
            const ULONGLONG now     = GetTickCount64();
            const ULONGLONG elapsed = now - w->startMs;
            if (elapsed >= kTotalMs) {
                KillTimer(hwnd, kTimerId);
                w->active = false;
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            // Two sin² bumps back-to-back: smooth, touches zero between them
            // for a clean "twice" feel.
            const ULONGLONG phase = elapsed % kPulseMs;
            const double    u     = static_cast<double>(phase) / kPulseMs;
            const double    s     = std::sin(3.14159265358979 * u);
            const double    intensity = s * s;
            const BYTE      alpha = static_cast<BYTE>(
                std::lround(intensity * 255.0));

            BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
            POINT srcPt{0, 0};
            SIZE  sz{w->width, w->height};
            POINT pos{w->rect.left, w->rect.top};
            UpdateLayeredWindow(hwnd, nullptr, &pos, &sz, w->memDc, &srcPt, 0,
                                &bf, ULW_ALPHA);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace monitour::overlay

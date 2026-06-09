#include "DebugOverlay.h"

#include <cwchar>

namespace monitour::overlay {

namespace {

constexpr wchar_t kClassName[] = L"MonitourDebugOverlay";
constexpr UINT    WM_OVERLAY_UPDATE = WM_APP + 10;

constexpr int kMargin    = 10;   // inner text padding
constexpr int kOriginX   = 12;   // offset from the monitor's left edge
constexpr int kOriginY   = 12;   // offset from the monitor's top edge
constexpr int kWidth     = 300;  // fixed box width
constexpr BYTE kAlpha    = 215;  // whole-window opacity (semi-transparent)

constexpr COLORREF kBackground = RGB(18, 18, 22);
constexpr COLORREF kTextColor  = RGB(225, 225, 230);
constexpr COLORREF kWarnColor  = RGB(255, 190, 70);

}  // namespace

DebugOverlay::~DebugOverlay() { destroy(); }

bool DebugOverlay::create(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &DebugOverlay::wndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // ignore "already registered" on re-create

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        kClassName, L"Monitour debug", WS_POPUP,
        kOriginX, kOriginY, kWidth, 120,
        nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;

    // Position relative to the primary monitor's work area.
    if (HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            SetWindowPos(hwnd_, HWND_TOPMOST,
                         mi.rcWork.left + kOriginX, mi.rcWork.top + kOriginY,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    font_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, FF_DONTCARE, L"Consolas");

    SetLayeredWindowAttributes(hwnd_, 0, kAlpha, LWA_ALPHA);
    recomposeAndResize();
    // Hidden by default; the user reveals it from the tray menu.
    return true;
}

void DebugOverlay::setVisible(bool visible) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool DebugOverlay::isVisible() const noexcept {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void DebugOverlay::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

void DebugOverlay::update(const Stats& s) {
    {
        std::lock_guard lock{mutex_};
        stats_ = s;
    }
    // Coalesce: only post if no repaint is already queued.
    if (hwnd_ && !updatePending_.exchange(true)) {
        PostMessageW(hwnd_, WM_OVERLAY_UPDATE, 0, 0);
    }
}

std::wstring DebugOverlay::compose() const {
    // Caller holds mutex_.
    const Stats& s = stats_;
    wchar_t buf[1024];
    _snwprintf_s(
        buf, _countof(buf), _TRUNCATE,
        L"Monitour debug\n"
        L"state    : %s%s\n"
        L"progress : %d%%\n"
        L"inference: %s\n"
        L"face     : %s\n"
        L"yaw/pitch: %.1f\u00b0 / %.1f\u00b0\n"
        L"vel      : %.0f\u00b0/s\n"
        L"conf     : %.2f\n"
        L"inputs   : %llu (kept %llu)\n"
        L"screens  : %zu\n"
        L"samples  : %llu / %llu\n"
        L"selfAcc  : %.0f%%\n"
        L"%s",
        s.active ? L"ACTIVE" : L"learning",
        s.teaching ? L" [TEACHING]" : L"",
        s.progressPct,
        s.inferenceOn ? L"on" : L"OFF",
        s.faceFound ? L"detected" : L"none",
        s.yaw, s.pitch, s.velocity, s.confidence,
        static_cast<unsigned long long>(s.inputs),
        static_cast<unsigned long long>(s.accepted),
        s.screens,
        static_cast<unsigned long long>(s.sampleCount),
        static_cast<unsigned long long>(s.minSamples),
        s.selfAccuracy * 100.0,
        s.reason.empty() ? L"progressing" : s.reason.c_str());
    std::wstring out{buf};

    // Live classifier feedback: predicted/committed line + per-monitor bar
    // chart. The bar chart is the key UI for teaching the calibrator on an
    // asymmetric layout \u2014 the user can see the laptop's score rise as they
    // look at it, even when the classifier hasn't (yet) crossed the margin.
    if (!s.perMonitor.empty()) {
        wchar_t line[160];
        auto labelOf = [](HMONITOR mon) -> std::wstring {
            if (!mon) return L"\u2014";
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) return mi.szDevice;
            wchar_t fb[32];
            _snwprintf_s(fb, _countof(fb), _TRUNCATE, L"%p", mon);
            return fb;
        };
        _snwprintf_s(line, _countof(line), _TRUNCATE,
                     L"\npredicted: %s\ncommitted: %s\n",
                     labelOf(s.predicted).c_str(),
                     labelOf(s.committed).c_str());
        out += line;

        // Normalise per-monitor scores to [0,1] for the bar chart. logScore
        // is unbounded; min-max within the snapshot is the simplest mapping.
        double lo = s.perMonitor.front().logScore;
        double hi = lo;
        for (const auto& pm : s.perMonitor) {
            lo = (std::min)(lo, pm.logScore);
            hi = (std::max)(hi, pm.logScore);
        }
        const double span = (hi - lo) > 1e-9 ? (hi - lo) : 1.0;
        for (const auto& pm : s.perMonitor) {
            const double t = (pm.logScore - lo) / span;
            const int filled = static_cast<int>(t * 8.0 + 0.5);
            wchar_t bar[10];
            for (int i = 0; i < 8; ++i)
                bar[i] = (i < filled) ? L'\u2588' : L'\u2591';
            bar[8] = L'\0';
            _snwprintf_s(line, _countof(line), _TRUNCATE,
                         L"  [%s] %s n=%llu\n",
                         bar, labelOf(pm.monitor).c_str(),
                         static_cast<unsigned long long>(pm.samples));
            out += line;
        }
    }
    return out;
}

void DebugOverlay::recomposeAndResize() {
    {
        std::lock_guard lock{mutex_};
        text_ = compose();
    }
    if (!hwnd_) return;

    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = font_ ? SelectObject(dc, font_) : nullptr;
    RECT r{0, 0, kWidth - 2 * kMargin, 0};
    {
        std::lock_guard lock{mutex_};
        DrawTextW(dc, text_.c_str(), -1, &r,
                  DT_CALCRECT | DT_LEFT | DT_NOPREFIX | DT_WORDBREAK);
    }
    if (old) SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);

    const int height = r.bottom - r.top + 2 * kMargin;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, kWidth, height,
                 SWP_NOMOVE | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void DebugOverlay::onPaint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);

    RECT client{};
    GetClientRect(hwnd_, &client);

    HBRUSH bg = CreateSolidBrush(kBackground);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    HGDIOBJ oldFont = font_ ? SelectObject(dc, font_) : nullptr;
    SetBkMode(dc, TRANSPARENT);

    std::wstring text;
    bool warn = false;
    {
        std::lock_guard lock{mutex_};
        text = text_;
        warn = !stats_.active &&
               (!stats_.inferenceOn || !stats_.reason.empty());
    }
    SetTextColor(dc, warn ? kWarnColor : kTextColor);

    RECT r = client;
    r.left   += kMargin;
    r.top    += kMargin;
    r.right  -= kMargin;
    DrawTextW(dc, text.c_str(), -1, &r,
              DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);

    if (oldFont) SelectObject(dc, oldFont);
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK DebugOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* self = reinterpret_cast<DebugOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
        case WM_OVERLAY_UPDATE:
            self->updatePending_.store(false);
            self->recomposeAndResize();
            return 0;
        case WM_PAINT:
            self->onPaint();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace monitour::overlay

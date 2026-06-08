#include "TrayIcon.h"

#include <mmsystem.h>

#include <cmath>
#include <cstdio>

#include "util/Logging.h"

namespace monitour::tray {

namespace {
constexpr UINT WM_TRAYICON   = WM_APP + 1;
constexpr UINT WM_SET_STATUS = WM_APP + 2;
constexpr UINT WM_SET_PROGRESS = WM_APP + 3;
constexpr UINT IDM_TOGGLE  = 1001;
constexpr UINT IDM_EXIT    = 1002;
constexpr UINT IDM_DEBUG_STATS = 1003;
constexpr UINT TRAY_UID    = 0xC0DE;
constexpr wchar_t kClass[] = L"MonitourTrayMessageWindow";

TrayIcon* g_self = nullptr;

// Builds a small anti-aliased filled-circle icon in the given color. Used to
// signal passive (yellow) vs. active (green) mode in the notification area.
HICON makeDotIcon(BYTE r, BYTE g, BYTE b) {
    const int sz = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size        = sizeof(BITMAPV5HEADER);
    bi.bV5Width       = sz;
    bi.bV5Height      = -sz;   // top-down
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }

    auto* px = static_cast<uint32_t*>(bits);
    const double c = (sz - 1) / 2.0;
    const double radius = sz / 2.0 - 1.0;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            const double dx = x - c, dy = y - c;
            const double edge = radius - std::sqrt(dx * dx + dy * dy);
            const double cov = edge >= 1.0 ? 1.0 : (edge <= 0.0 ? 0.0 : edge);
            const BYTE a = static_cast<BYTE>(cov * 255.0 + 0.5);
            px[y * sz + x] = (static_cast<uint32_t>(a) << 24) |
                             (static_cast<uint32_t>(r) << 16) |
                             (static_cast<uint32_t>(g) << 8) |
                             static_cast<uint32_t>(b);
        }
    }

    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
}

TrayIcon::TrayIcon() { g_self = this; }
TrayIcon::~TrayIcon() {
    destroy();
    if (g_self == this) g_self = nullptr;
}

bool TrayIcon::create(HINSTANCE hInstance, Callbacks cb) {
    cb_ = std::move(cb);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &TrayIcon::wndProc;
    wc.hInstance   = hInstance;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kClass, L"Monitour", 0,
                            0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, hInstance, this);
    if (!hwnd_) {
        log::error(L"TrayIcon: CreateWindowEx failed: {}", GetLastError());
        return false;
    }

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd   = hwnd_;
    nid_.uID    = TRAY_UID;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    iconPassive_ = makeDotIcon(240, 200, 0);   // yellow: learning
    iconActive_  = makeDotIcon(40, 200, 80);    // green: active
    nid_.hIcon  = iconPassive_ ? iconPassive_ : LoadIconW(nullptr, IDI_APPLICATION);
    updateTip();
    Shell_NotifyIconW(NIM_ADD, &nid_);
    return true;
}

void TrayIcon::destroy() {
    if (hwnd_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (iconPassive_) { DestroyIcon(iconPassive_); iconPassive_ = nullptr; }
    if (iconActive_)  { DestroyIcon(iconActive_);  iconActive_  = nullptr; }
}

void TrayIcon::updateTip() {
    const bool paused = cb_.isPaused && cb_.isPaused();
    wchar_t buf[128];
    if (paused) {
        wcscpy_s(buf, L"Monitour (paused)");
    } else if (status_ == Status::Active) {
        wcscpy_s(buf, L"Monitour (active)");
    } else {
        std::wstring detail;
        {
            std::lock_guard lock{detailMutex_};
            detail = progressDetail_;
        }
        if (detail.empty()) {
            swprintf_s(buf, L"Monitour (learning\u2026 %d%%)", progressPct_);
        } else {
            // szTip caps at 128 wchars; keep the headline, then as much of the
            // detail as fits on a second line. Use _TRUNCATE so an over-long
            // message is clipped rather than aborting the process.
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                         L"Monitour (learning\u2026 %d%%)\n%s",
                         progressPct_, detail.c_str());
        }
    }
    buf[_countof(buf) - 1] = L'\0';
    wcscpy_s(nid_.szTip, buf);
}

void TrayIcon::refresh() {
    if (!hwnd_) return;
    updateTip();
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::postStatus(Status s) {
    if (hwnd_) PostMessageW(hwnd_, WM_SET_STATUS, static_cast<WPARAM>(s), 0);
}

void TrayIcon::postProgress(int percent, std::wstring detail) {
    {
        std::lock_guard lock{detailMutex_};
        progressDetail_ = std::move(detail);
    }
    if (hwnd_) PostMessageW(hwnd_, WM_SET_PROGRESS,
                            static_cast<WPARAM>(percent), 0);
}

void TrayIcon::applyProgress(int percent) {
    if (!hwnd_) return;
    progressPct_ = percent;
    if (status_ == Status::Passive) {
        updateTip();
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }
}

void TrayIcon::applyStatus(Status s) {
    if (!hwnd_) return;
    const bool becameActive = (s == Status::Active && status_ != Status::Active);
    status_ = s;
    HICON icon = (s == Status::Active) ? iconActive_ : iconPassive_;
    if (icon) nid_.hIcon = icon;
    updateTip();
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    if (becameActive) {
        // Audible cue that learning is complete and focus-following is live.
        PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC);
        log::info(L"Tray: entered ACTIVE mode");
    }
}

LRESULT CALLBACK TrayIcon::wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_self && h == g_self->hwnd_) return g_self->handle(msg, w, l);
    return DefWindowProcW(h, msg, w, l);
}

LRESULT TrayIcon::handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON: {
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                showContextMenu(pt);
            } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                if (cb_.onTogglePause) cb_.onTogglePause();
                refresh();
            }
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDM_TOGGLE:
                    if (cb_.onTogglePause) cb_.onTogglePause();
                    refresh();
                    return 0;
                case IDM_DEBUG_STATS:
                    if (cb_.onToggleDebugStats) cb_.onToggleDebugStats();
                    return 0;
                case IDM_EXIT:
                    if (cb_.onQuit) cb_.onQuit();
                    return 0;
            }
            break;
        }
        case WM_SET_STATUS:
            applyStatus(static_cast<Status>(wParam));
            return 0;
        case WM_SET_PROGRESS:
            applyProgress(static_cast<int>(wParam));
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void TrayIcon::showContextMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    bool paused = cb_.isPaused && cb_.isPaused();
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE, paused ? L"Resume" : L"Pause");
    if (cb_.onToggleDebugStats) {
        const bool visible = cb_.isDebugStatsVisible && cb_.isDebugStatsVisible();
        AppendMenuW(menu, MF_STRING | (visible ? MF_CHECKED : MF_UNCHECKED),
                    IDM_DEBUG_STATS, L"Show debug stats");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

}  // namespace monitour::tray

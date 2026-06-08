#pragma once
#include <Windows.h>
#include <shellapi.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace monitour::tray {

// Minimal Shell_NotifyIcon wrapper. Owns a hidden message-only window for
// the icon's callback, exposes a context menu, and surfaces:
//   - onTogglePause()  — Ctrl+Alt+M or "Pause" menu item
//   - onQuit()         — "Exit" menu item
//
// Lives on the main UI thread along with the global hotkey and the
// SetWinEventHook callback dispatch.
class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> onTogglePause;
        std::function<void()> onQuit;
        std::function<bool()> isPaused;
        // Optional: when set, a checkable "Show debug stats" item appears in
        // the context menu. isDebugStatsVisible reports the current state.
        std::function<void()> onToggleDebugStats;
        std::function<bool()> isDebugStatsVisible;
        // Optional: when set, a checkable "Focus glow" item appears.
        std::function<void()> onToggleFocusGlow;
        std::function<bool()> isFocusGlowEnabled;
    };

    // Passive: still learning which monitor the user faces (yellow dot).
    // Active:  calibrated and switching focus (green dot).
    enum class Status { Passive, Active };

    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool create(HINSTANCE hInstance, Callbacks cb);
    void destroy();
    void refresh();

    // Thread-safe: posts a status change to the tray window so the icon is
    // updated (and the activation sound played) on the owning UI thread.
    void postStatus(Status s);

    // Thread-safe: posts learning progress shown in the tooltip while in
    // passive mode. `percent` is 0..100; `detail` is an optional short
    // per-monitor breakdown appended on its own line.
    void postProgress(int percent, std::wstring detail = {});

    HWND messageWindow() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void showContextMenu(POINT pt);
    void applyStatus(Status s);
    void applyProgress(int percent);
    void updateTip();

    HWND       hwnd_{nullptr};
    NOTIFYICONDATAW nid_{};
    Callbacks  cb_{};
    Status     status_{Status::Passive};
    int        progressPct_{0};
    std::mutex   detailMutex_;
    std::wstring progressDetail_;
    HICON      iconPassive_{nullptr};
    HICON      iconActive_{nullptr};
};

}  // namespace monitour::tray

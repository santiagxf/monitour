#pragma once
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace monitour::overlay {

// A small, semi-transparent, click-through stats box pinned to the top-left of
// the primary monitor. It is intended purely as a debugging aid and is only
// created in Debug builds (see the MONITOUR_DEBUG_OVERLAY guard in main).
//
// Threading: create()/destroy()/runs on the UI (message-loop) thread; update()
// is safe to call from the inference worker — it stores the snapshot and posts
// a coalesced repaint to the window.
class DebugOverlay {
public:
    struct Stats {
        bool     active{false};
        int      progressPct{0};
        bool     inferenceOn{false};
        uint64_t inputs{0};
        uint64_t accepted{0};
        size_t   screens{0};
        uint64_t sampleCount{0};
        uint64_t minSamples{0};
        double   separationDeg{0.0};
        double   minSeparationDeg{0.0};
        double   yaw{0.0};
        double   pitch{0.0};
        double   velocity{0.0};
        double   confidence{0.0};
        bool     faceFound{false};
        std::wstring reason;   // empty → progressing normally
    };

    DebugOverlay() = default;
    ~DebugOverlay();

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    bool create(HINSTANCE hinst);
    void destroy();

    // Show/hide the window. Call from the UI thread.
    void setVisible(bool visible);
    bool isVisible() const noexcept;

    // Thread-safe: snapshot the stats and request a repaint.
    void update(const Stats& s);

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void onPaint();
    void recomposeAndResize();
    std::wstring compose() const;

    HWND  hwnd_{nullptr};
    HFONT font_{nullptr};
    std::mutex mutex_;
    Stats stats_{};
    std::wstring text_;
    std::atomic<bool> updatePending_{false};
};

}  // namespace monitour::overlay

#pragma once
#include <Windows.h>

#include <mutex>
#include <vector>

namespace monitour::overlay {

// A brief, click-through, magenta→violet glow that pulses twice (~700 ms total)
// on the border of a monitor that just received Monitour-driven focus. Purely
// cosmetic confirmation that the head-pose switch landed.
//
// Threading: create()/destroy()/onDisplayChange() must run on the UI
// (message-loop) thread. flash() is safe to call from the inference worker —
// it only posts a message to the per-monitor window.
class FocusGlow {
public:
    FocusGlow() = default;
    ~FocusGlow();

    FocusGlow(const FocusGlow&) = delete;
    FocusGlow& operator=(const FocusGlow&) = delete;

    bool create(HINSTANCE hinst);
    void destroy();

    // Rebuild the per-monitor windows after a display arrangement change.
    void onDisplayChange();

    // Trigger the double pulse on the window covering `monitor`. No-op if the
    // monitor isn't known or a pulse is already in flight on it.
    void flash(HMONITOR monitor);

private:
    struct Window;

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

    void rebuildWindows();
    void teardownWindows();
    static void renderBitmap(Window& w);

    HINSTANCE          hinst_{nullptr};
    std::mutex         mutex_;            // guards windows_ vs flash() lookups
    std::vector<Window*> windows_;        // one per active monitor
};

}  // namespace monitour::overlay

#pragma once
#include <Windows.h>

#include <mutex>
#include <unordered_map>
#include <vector>

namespace monitour::focus {

// Tracks per-monitor most-recently-foregrounded windows using a low-level
// EVENT_SYSTEM_FOREGROUND hook (WINEVENT_OUTOFCONTEXT, no DLL injection).
//
// On a focus request for a given HMONITOR, returns the freshest live, visible,
// candidate HWND on that monitor. Stale or destroyed HWNDs are pruned lazily.
class MonitorMru {
public:
    MonitorMru();
    ~MonitorMru();

    MonitorMru(const MonitorMru&) = delete;
    MonitorMru& operator=(const MonitorMru&) = delete;

    void start();
    void stop();

    // Returns the freshest focusable HWND on `monitor`, or nullptr.
    HWND pickForMonitor(HMONITOR monitor);

    // Rebuilds monitor set after WM_DISPLAYCHANGE. Existing MRU stacks are
    // remapped by re-querying MonitorFromWindow per HWND.
    void onDisplayChange();

private:
    static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event,
                                      HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD eventThread, DWORD eventTime);

    void onForegroundChanged(HWND hwnd);

    HWINEVENTHOOK hook_{nullptr};
    std::mutex    mutex_;
    // MRU stack per monitor: front = most-recently-focused.
    std::unordered_map<HMONITOR, std::vector<HWND>> mru_;
};

// Singleton-ish accessor used by the static thunk.
MonitorMru& instance();

}  // namespace monitour::focus

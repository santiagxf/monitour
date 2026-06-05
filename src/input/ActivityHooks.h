#pragma once
#include <Windows.h>

#include <atomic>
#include <chrono>

namespace monitour::input {

// Low-level mouse + keyboard hooks that report the most recent user-input
// timestamps and cursor monitor. Used by the orchestrator to:
//   - suppress focus changes for 1.5 s after any user input (prevents
//     stealing focus mid-sentence),
//   - gate calibration evidence (need real clicks/keystrokes on the monitor
//     where the cursor is parked, not idle hover).
//
// These are global hooks; they must be installed on the message-loop thread.
class ActivityHooks {
public:
    ActivityHooks();
    ~ActivityHooks();

    ActivityHooks(const ActivityHooks&) = delete;
    ActivityHooks& operator=(const ActivityHooks&) = delete;

    void install();
    void uninstall();

    // Time since any keystroke or mouse-button event.
    std::chrono::milliseconds sinceLastUserInput() const;

    // Time the cursor has been on the current monitor without leaving it.
    std::chrono::milliseconds cursorParkedDuration() const;
    HMONITOR                  cursorMonitor() const;

    // Was the most recent button/keystroke event delivered to a window on
    // `monitor`? Used by the calibrator's evidence gate.
    bool lastInputWasOnMonitor(HMONITOR monitor) const;

    // The monitor the most recent real input targeted: for keystrokes this is
    // the foreground window's monitor (where the text lands); for clicks it is
    // the monitor under the cursor. nullptr until the first input.
    HMONITOR lastInputMonitor() const;

    // Monotonic counter incremented on every real keystroke / mouse-button
    // event. The orchestrator records one calibration sample per new value, so
    // typing and clicking both feed the learner exactly once per event.
    uint64_t inputSeq() const;

private:
    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc   (int code, WPARAM wParam, LPARAM lParam);

    void onKey();
    void onMouseEvent(WPARAM event, POINT pt);

    HHOOK kbdHook_{nullptr};
    HHOOK mouseHook_{nullptr};

    std::atomic<int64_t> lastInputTickQpc_{0};
    std::atomic<int64_t> cursorEnteredMonitorQpc_{0};
    std::atomic<HMONITOR> currentCursorMonitor_{nullptr};
    std::atomic<HMONITOR> lastInputMonitor_{nullptr};
    std::atomic<uint64_t> inputSeq_{0};
};

ActivityHooks& instance();

}  // namespace monitour::input

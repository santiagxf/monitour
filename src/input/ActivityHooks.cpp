#include "ActivityHooks.h"

#include "util/Logging.h"

namespace monitour::input {

namespace {
ActivityHooks* g_instance = nullptr;

int64_t qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t qpcFreq() {
    static int64_t freq = []{
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

std::chrono::milliseconds qpcToMs(int64_t ticks) {
    return std::chrono::milliseconds{(ticks * 1000) / qpcFreq()};
}

}  // namespace

ActivityHooks& instance() { return *g_instance; }

ActivityHooks::ActivityHooks() { g_instance = this; }

ActivityHooks::~ActivityHooks() {
    uninstall();
    if (g_instance == this) g_instance = nullptr;
}

void ActivityHooks::install() {
    if (kbdHook_ || mouseHook_) return;
    kbdHook_   = SetWindowsHookExW(WH_KEYBOARD_LL, &keyboardProc, GetModuleHandleW(nullptr), 0);
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL,    &mouseProc,    GetModuleHandleW(nullptr), 0);
    if (!kbdHook_ || !mouseHook_) {
        log::error(L"ActivityHooks: SetWindowsHookExW failed: {}", GetLastError());
    }
}

void ActivityHooks::uninstall() {
    if (kbdHook_)   { UnhookWindowsHookEx(kbdHook_);   kbdHook_   = nullptr; }
    if (mouseHook_) { UnhookWindowsHookEx(mouseHook_); mouseHook_ = nullptr; }
}

LRESULT CALLBACK ActivityHooks::keyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            g_instance->onKey();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK ActivityHooks::mouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_instance) {
        auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (info) g_instance->onMouseEvent(wParam, info->pt);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void ActivityHooks::onKey() {
    int64_t now = qpcNow();
    lastInputTickQpc_.store(now, std::memory_order_relaxed);
    // Keystrokes land on the focused window, so the foreground window's monitor
    // is the screen the user is actually working on — a strong gaze cue, and
    // independent of where the mouse pointer happens to rest.
    HMONITOR mon = nullptr;
    if (HWND fg = GetForegroundWindow()) {
        mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) {
        POINT pt{};
        if (GetCursorPos(&pt)) {
            mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        }
    }
    lastInputMonitor_.store(mon, std::memory_order_relaxed);
    inputSeq_.fetch_add(1, std::memory_order_relaxed);
}

void ActivityHooks::onMouseEvent(WPARAM event, POINT pt) {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    HMONITOR prev = currentCursorMonitor_.exchange(mon, std::memory_order_relaxed);
    int64_t now = qpcNow();
    if (prev != mon) {
        cursorEnteredMonitorQpc_.store(now, std::memory_order_relaxed);
    }
    if (event == WM_LBUTTONDOWN || event == WM_RBUTTONDOWN || event == WM_MBUTTONDOWN ||
        event == WM_XBUTTONDOWN) {
        lastInputTickQpc_.store(now, std::memory_order_relaxed);
        lastInputMonitor_.store(mon, std::memory_order_relaxed);
        inputSeq_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::chrono::milliseconds ActivityHooks::sinceLastUserInput() const {
    int64_t last = lastInputTickQpc_.load(std::memory_order_relaxed);
    if (!last) return std::chrono::milliseconds::max();
    return qpcToMs(qpcNow() - last);
}

std::chrono::milliseconds ActivityHooks::cursorParkedDuration() const {
    int64_t since = cursorEnteredMonitorQpc_.load(std::memory_order_relaxed);
    if (!since) return std::chrono::milliseconds{0};
    return qpcToMs(qpcNow() - since);
}

HMONITOR ActivityHooks::cursorMonitor() const {
    return currentCursorMonitor_.load(std::memory_order_relaxed);
}

bool ActivityHooks::lastInputWasOnMonitor(HMONITOR monitor) const {
    return lastInputMonitor_.load(std::memory_order_relaxed) == monitor;
}

HMONITOR ActivityHooks::lastInputMonitor() const {
    return lastInputMonitor_.load(std::memory_order_relaxed);
}

uint64_t ActivityHooks::inputSeq() const {
    return inputSeq_.load(std::memory_order_relaxed);
}

}  // namespace monitour::input

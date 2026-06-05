#include "MonitorMru.h"

#include <algorithm>

#include "WindowEnumerator.h"
#include "util/Logging.h"

namespace monitour::focus {

namespace {
MonitorMru* g_instance = nullptr;
}

MonitorMru& instance() {
    return *g_instance;
}

MonitorMru::MonitorMru() {
    g_instance = this;
}

MonitorMru::~MonitorMru() {
    stop();
    if (g_instance == this) g_instance = nullptr;
}

void MonitorMru::start() {
    if (hook_) return;
    hook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, &MonitorMru::winEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook_) {
        log::error(L"SetWinEventHook failed: {}", GetLastError());
    }

    // Prime MRU stacks with current top-level windows in Z-order.
    auto windows = enumerateCandidateWindows();
    std::lock_guard lock{mutex_};
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        if (!it->monitor) continue;
        mru_[it->monitor].insert(mru_[it->monitor].begin(), it->hwnd);
    }
}

void MonitorMru::stop() {
    if (hook_) {
        UnhookWinEvent(hook_);
        hook_ = nullptr;
    }
}

void CALLBACK MonitorMru::winEventProc(HWINEVENTHOOK, DWORD event,
                                       HWND hwnd, LONG idObject, LONG,
                                       DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !hwnd) {
        return;
    }
    if (g_instance) g_instance->onForegroundChanged(hwnd);
}

void MonitorMru::onForegroundChanged(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon) return;

    std::lock_guard lock{mutex_};
    auto& stack = mru_[mon];
    auto it = std::find(stack.begin(), stack.end(), hwnd);
    if (it != stack.end()) stack.erase(it);
    stack.insert(stack.begin(), hwnd);
    if (stack.size() > 64) stack.resize(64);

    // Also purge this HWND from other monitors' stacks (window moved screens).
    for (auto& [other_mon, other_stack] : mru_) {
        if (other_mon == mon) continue;
        auto e = std::remove(other_stack.begin(), other_stack.end(), hwnd);
        other_stack.erase(e, other_stack.end());
    }
}

HWND MonitorMru::pickForMonitor(HMONITOR monitor) {
    std::lock_guard lock{mutex_};
    auto it = mru_.find(monitor);
    if (it == mru_.end()) return nullptr;

    auto& stack = it->second;
    while (!stack.empty()) {
        HWND h = stack.front();
        if (!IsWindow(h) || !IsWindowVisible(h)) {
            stack.erase(stack.begin());
            continue;
        }
        // Re-check current monitor in case the window was moved.
        if (MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != monitor) {
            stack.erase(stack.begin());
            continue;
        }
        return h;
    }
    return nullptr;
}

void MonitorMru::onDisplayChange() {
    std::lock_guard lock{mutex_};
    decltype(mru_) rebuilt;
    for (auto& [_, stack] : mru_) {
        for (HWND h : stack) {
            if (!IsWindow(h)) continue;
            HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            if (!m) continue;
            rebuilt[m].push_back(h);
        }
    }
    mru_ = std::move(rebuilt);
}

}  // namespace monitour::focus

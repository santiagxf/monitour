#include "ForegroundSetter.h"

#include "WindowEnumerator.h"
#include "util/Logging.h"

namespace monitour::focus {

bool ForegroundSetter::setForeground(HWND target) {
    if (!target || !IsWindow(target)) return false;
    HWND current = GetForegroundWindow();
    if (current == target) return true;

    if (!canFocusTarget(target)) {
        log::debug(L"Skipping HWND 0x{:x}: elevated and we are not", reinterpret_cast<uintptr_t>(target));
        return false;
    }

    DWORD targetTid = GetWindowThreadProcessId(target, nullptr);
    DWORD currentTid = current ? GetWindowThreadProcessId(current, nullptr) : 0;
    DWORD selfTid = GetCurrentThreadId();

    // Loosen the foreground lock for our own process.
    AllowSetForegroundWindow(ASFW_ANY);

    bool attached = false;
    if (currentTid && currentTid != selfTid) {
        attached = AttachThreadInput(selfTid, currentTid, TRUE) != 0;
    }

    bool ok = false;
    if (target) {
        if (IsIconic(target)) {
            ShowWindow(target, SW_RESTORE);
        }
        BringWindowToTop(target);
        ok = SetForegroundWindow(target) != 0;
        if (!ok) {
            // Backup: SetWindowPos with TOPMOST flicker is a classic workaround
            // when AttachThreadInput is blocked.
            SetWindowPos(target, HWND_TOPMOST,   0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ok = SetForegroundWindow(target) != 0;
        }
    }

    if (attached) {
        AttachThreadInput(selfTid, currentTid, FALSE);
    }

    if (!ok) {
        log::warn(L"SetForegroundWindow failed for HWND 0x{:x}: {}",
                  reinterpret_cast<uintptr_t>(target), GetLastError());
    }
    (void)targetTid;
    return ok;
}

}  // namespace monitour::focus

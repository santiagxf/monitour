#include "WindowEnumerator.h"

#include <dwmapi.h>
#include <psapi.h>

#include "util/Logging.h"

#pragma comment(lib, "dwmapi.lib")

namespace monitour::focus {

namespace {

bool isCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

bool isElevated(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return true;  // can't open → assume elevated (we can't focus it).

    HANDLE token = nullptr;
    bool elevated = false;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD sz = 0;
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &sz)) {
            elevated = e.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    CloseHandle(proc);
    return elevated;
}

bool isCandidate(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (isCloaked(hwnd)) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    // Skip shell/desktop surfaces.
    if (wcscmp(cls, L"Progman") == 0)         return false;
    if (wcscmp(cls, L"WorkerW") == 0)         return false;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0)   return false;
    if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) return false;
    return true;
}

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lparam);
    if (!isCandidate(hwnd)) return TRUE;
    WindowInfo w;
    w.hwnd = hwnd;
    GetWindowRect(hwnd, &w.bounds);
    w.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    GetWindowThreadProcessId(hwnd, &w.processId);
    w.cloaked = false;  // filtered above
    w.elevated = isElevated(w.processId);
    out->push_back(w);
    return TRUE;
}

}  // namespace

std::vector<WindowInfo> enumerateCandidateWindows() {
    std::vector<WindowInfo> out;
    out.reserve(64);
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

bool canFocusTarget(HWND target) {
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (!pid) return false;
    bool targetElevated = isElevated(pid);
    if (!targetElevated) return true;

    // We're elevated iff our own token says so.
    HANDLE me = GetCurrentProcess();
    HANDLE token = nullptr;
    bool selfElevated = false;
    if (OpenProcessToken(me, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD sz = 0;
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &sz)) {
            selfElevated = e.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    return selfElevated;
}

}  // namespace monitour::focus

#pragma once
#include <Windows.h>

#include <vector>

namespace monitour::focus {

struct WindowInfo {
    HWND      hwnd{nullptr};
    HMONITOR  monitor{nullptr};
    DWORD     processId{0};
    RECT      bounds{};
    bool      cloaked{false};
    bool      elevated{false};
};

// Returns top-level, visible, non-tool, non-cloaked windows ordered as
// EnumWindows returned them (≈ current Z-order, top-most first).
std::vector<WindowInfo> enumerateCandidateWindows();

// Returns true if the running process can SetForegroundWindow on `target`.
// In practice this means same elevation level or higher.
bool canFocusTarget(HWND target);

}  // namespace monitour::focus

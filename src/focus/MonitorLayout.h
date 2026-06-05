#pragma once
#include <Windows.h>

#include <string>
#include <vector>

namespace monitour::focus {

// A single display as Windows reports it, in virtual-desktop coordinates.
struct MonitorInfo {
    HMONITOR     handle{nullptr};
    RECT         rect{};          // rcMonitor, virtual-desktop pixels
    std::wstring device;          // szDevice, e.g. "\\.\DISPLAY1"
    bool         primary{false};
};

// Enumerates all active monitors, ordered left-to-right by horizontal center.
// Windows already knows the physical arrangement, so this is the authoritative
// source for "where each screen sits" — no manual configuration needed.
std::vector<MonitorInfo> enumerateMonitorsLeftToRight();

}  // namespace monitour::focus

#include "MonitorLayout.h"

#include <algorithm>

namespace monitour::focus {

namespace {

BOOL CALLBACK enumProc(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        MonitorInfo info{};
        info.handle  = mon;
        info.rect    = mi.rcMonitor;
        info.device  = mi.szDevice;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        out->push_back(info);
    }
    return TRUE;
}

}  // namespace

std::vector<MonitorInfo> enumerateMonitorsLeftToRight() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, enumProc, reinterpret_cast<LPARAM>(&monitors));

    std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        const long centerA = (a.rect.left + a.rect.right) / 2;
        const long centerB = (b.rect.left + b.rect.right) / 2;
        if (centerA != centerB) return centerA < centerB;
        return a.rect.top < b.rect.top;  // stable tie-break for stacked displays
    });

    return monitors;
}

}  // namespace monitour::focus

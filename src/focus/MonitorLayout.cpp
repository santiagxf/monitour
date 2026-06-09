#include "MonitorLayout.h"

#include <algorithm>
#include <cmath>

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

std::vector<AngularExtent>
computeAngularExtents(const std::vector<MonitorInfo>& layout,
                      double viewingCm) {
    // Convert pixel rects to physical centimetres using the assumed 96 DPI
    // baseline (≈ 0.02646 cm per pixel). We don't query per-monitor DPI here
    // because virtual-desktop pixel coordinates are already DPI-normalised by
    // Windows in most multi-monitor setups. An error of ±20% in the factor
    // only changes the prior's spread by ±20% — well inside the histogram's
    // resolving power.
    constexpr double kCmPerPixel = 2.54 / 96.0;

    // The user's eye is at the centroid of all monitor centers (in virtual-
    // desktop coords), projected `viewingCm` forward. Each screen's center
    // vector relative to the eye is (Δx, −Δy, viewing) — +y in screen coords
    // is downward, +pitch in head-pose is upward, hence the sign flip.
    double cxSum = 0.0, cySum = 0.0;
    for (const auto& m : layout) {
        cxSum += (m.rect.left + m.rect.right) * 0.5;
        cySum += (m.rect.top + m.rect.bottom) * 0.5;
    }
    const size_t n = layout.size();
    const double eyeCx =
        n > 0 ? cxSum / static_cast<double>(n) : 0.0;
    const double eyeCy =
        n > 0 ? cySum / static_cast<double>(n) : 0.0;

    constexpr double kRadToDeg = 57.29577951308232;

    std::vector<AngularExtent> out;
    out.reserve(layout.size());
    for (const auto& m : layout) {
        const double centerPxX = (m.rect.left + m.rect.right) * 0.5;
        const double centerPxY = (m.rect.top + m.rect.bottom) * 0.5;
        const double dxCm = (centerPxX - eyeCx) * kCmPerPixel;
        const double dyCm = (centerPxY - eyeCy) * kCmPerPixel;
        const double halfWidthCm =
            std::max<double>(1.0,
                             (m.rect.right - m.rect.left) * kCmPerPixel * 0.5);
        const double halfHeightCm =
            std::max<double>(1.0,
                             (m.rect.bottom - m.rect.top) * kCmPerPixel * 0.5);

        AngularExtent e{};
        e.yawCenterDeg   = std::atan2(dxCm, viewingCm) * kRadToDeg;
        e.pitchCenterDeg = -std::atan2(dyCm, viewingCm) * kRadToDeg;
        e.halfExtentYawDeg =
            std::atan2(halfWidthCm, viewingCm) * kRadToDeg;
        e.halfExtentPitchDeg =
            std::atan2(halfHeightCm, viewingCm) * kRadToDeg;
        out.push_back(e);
    }
    return out;
}

}  // namespace monitour::focus

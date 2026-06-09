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

// Geometric angular footprint of one monitor as seen from the user's eye.
// Computed from the monitor's pixel rect and an assumed Windows DPI of 96
// (pixels-per-inch fallback) together with an assumed viewing distance in cm.
// "halfExtent" is the half-angle from the screen center to either edge.
struct AngularExtent {
    double yawCenterDeg{0.0};
    double pitchCenterDeg{0.0};
    double halfExtentYawDeg{15.0};
    double halfExtentPitchDeg{10.0};
};

// `layout` is the result of enumerateMonitorsLeftToRight(). `viewingCm` is the
// assumed eye-to-screen distance for the user's primary working pose.
// Returns one AngularExtent per monitor in the same order as `layout`.
std::vector<AngularExtent>
computeAngularExtents(const std::vector<MonitorInfo>& layout,
                      double viewingCm = 60.0);

}  // namespace monitour::focus

#include <catch2/catch_test_macros.hpp>

#include "calibration/PassiveCalibrator.h"

using monitour::calibration::PassiveCalibrator;

namespace {
const HMONITOR M1 = reinterpret_cast<HMONITOR>(0x1);
const HMONITOR M2 = reinterpret_cast<HMONITOR>(0x2);
const HMONITOR M3 = reinterpret_cast<HMONITOR>(0x3);
}

TEST_CASE("classifier refuses during cold start", "[calibrator]") {
    PassiveCalibrator c;
    REQUIRE_FALSE(c.classify(0.0, 0.0, 1.0).has_value());
}

TEST_CASE("classifier refuses with only one monitor known", "[calibrator]") {
    PassiveCalibrator c;
    for (int i = 0; i < 50; ++i) c.recordEvidence(M1, 0.0, 0.0, 1.0);
    REQUIRE_FALSE(c.classify(0.0, 0.0, 1.0).has_value());
}

TEST_CASE("classifier converges with well-separated monitors", "[calibrator]") {
    PassiveCalibrator c;
    // Two monitors 40° apart in yaw (side-by-side), same pitch.
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    auto a = c.classify(-18.0, 0.0, 1.0);
    auto b = c.classify( 18.0, 0.0, 1.0);
    REQUIRE(a.has_value()); REQUIRE(*a == M1);
    REQUIRE(b.has_value()); REQUIRE(*b == M2);
}

TEST_CASE("classifier separates vertically-stacked monitors by pitch",
          "[calibrator]") {
    PassiveCalibrator c;
    // Same yaw (0°), separated only in pitch: M1 below (look down, −20°),
    // M2 above (look up, +20°). Pitch must be the discriminating axis.
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, 0.0, -20.0, 1.0);
        c.recordEvidence(M2, 0.0,  20.0, 1.0);
    }
    auto down = c.classify(0.0, -18.0, 1.0);
    auto up   = c.classify(0.0,  18.0, 1.0);
    REQUIRE(down.has_value()); REQUIRE(*down == M1);
    REQUIRE(up.has_value());   REQUIRE(*up == M2);
}

TEST_CASE("classifier returns nullopt for ambiguous yaw at midpoint", "[calibrator]") {
    PassiveCalibrator c;
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    auto mid = c.classify(0.0, 0.0, 1.0);
    REQUIRE_FALSE(mid.has_value());
}

TEST_CASE("classifier rejects an outlier far from every monitor", "[calibrator]") {
    PassiveCalibrator c;
    // Two screens side-by-side around pitch 0°. Looking sharply down at the
    // keyboard (pitch −45°) is far outside both learned ranges and must be
    // rejected, regardless of yaw — the data-driven replacement for a hardcoded
    // pitch limit.
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    REQUIRE_FALSE(c.classify(-20.0, -45.0, 1.0).has_value());
    REQUIRE_FALSE(c.classify( 20.0, -45.0, 1.0).has_value());
}

TEST_CASE("classifier rejects low face confidence", "[calibrator]") {
    PassiveCalibrator c;
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    auto r = c.classify(-20.0, 0.0, 0.1);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("3-monitor scenario", "[calibrator]") {
    PassiveCalibrator c;
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -25.0, 0.0, 1.0);
        c.recordEvidence(M2,   0.0, 0.0, 1.0);
        c.recordEvidence(M3,  25.0, 0.0, 1.0);
    }
    REQUIRE(c.classify(-25.0, 0.0, 1.0) == M1);
    REQUIRE(c.classify(  0.0, 0.0, 1.0) == M2);
    REQUIRE(c.classify( 25.0, 0.0, 1.0) == M3);
}

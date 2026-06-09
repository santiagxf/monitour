#include <catch2/catch_test_macros.hpp>

#include "calibration/PassiveCalibrator.h"

using monitour::calibration::PassiveCalibrator;

namespace {
const HMONITOR M1 = reinterpret_cast<HMONITOR>(0x1);
const HMONITOR M2 = reinterpret_cast<HMONITOR>(0x2);
const HMONITOR M3 = reinterpret_cast<HMONITOR>(0x3);

PassiveCalibrator::LayoutPrior makePrior(HMONITOR mon, double yaw, double pitch,
                                         double hExtYaw = 15.0,
                                         double hExtPitch = 10.0) {
    PassiveCalibrator::LayoutPrior p{};
    p.monitor = mon;
    p.yawDeg = yaw;
    p.pitchDeg = pitch;
    p.halfExtentYawDeg = hExtYaw;
    p.halfExtentPitchDeg = hExtPitch;
    return p;
}
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
    c.setLayoutPriors({makePrior(M1, -20.0, 0.0), makePrior(M2, 20.0, 0.0)});
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
    c.setLayoutPriors({makePrior(M1, 0.0, -20.0), makePrior(M2, 0.0, 20.0)});
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
    c.setLayoutPriors({makePrior(M1, -20.0, 0.0), makePrior(M2, 20.0, 0.0)});
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    auto mid = c.classify(0.0, 0.0, 1.0);
    REQUIRE_FALSE(mid.has_value());
}

TEST_CASE("classifier rejects an outlier far from every monitor", "[calibrator]") {
    PassiveCalibrator c;
    c.setLayoutPriors({makePrior(M1, -20.0, 0.0), makePrior(M2, 20.0, 0.0)});
    // Looking sharply down at the keyboard (pitch −45°) is far outside both
    // priors' pitch ranges and must be rejected via the prior-log floor.
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    REQUIRE_FALSE(c.classify(-20.0, -45.0, 1.0).has_value());
    REQUIRE_FALSE(c.classify( 20.0, -45.0, 1.0).has_value());
}

TEST_CASE("classifier rejects low face confidence", "[calibrator]") {
    PassiveCalibrator c;
    c.setLayoutPriors({makePrior(M1, -20.0, 0.0), makePrior(M2, 20.0, 0.0)});
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -20.0, 0.0, 1.0);
        c.recordEvidence(M2,  20.0, 0.0, 1.0);
    }
    auto r = c.classify(-20.0, 0.0, 0.1);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("3-monitor scenario", "[calibrator]") {
    PassiveCalibrator c;
    c.setLayoutPriors({makePrior(M1, -25.0, 0.0),
                       makePrior(M2,   0.0, 0.0),
                       makePrior(M3,  25.0, 0.0)});
    for (int i = 0; i < 200; ++i) {
        c.recordEvidence(M1, -25.0, 0.0, 1.0);
        c.recordEvidence(M2,   0.0, 0.0, 1.0);
        c.recordEvidence(M3,  25.0, 0.0, 1.0);
    }
    REQUIRE(c.classify(-25.0, 0.0, 1.0) == M1);
    REQUIRE(c.classify(  0.0, 0.0, 1.0) == M2);
    REQUIRE(c.classify( 25.0, 0.0, 1.0) == M3);
}

TEST_CASE("wide main screen plus narrow adjacent laptop", "[calibrator]") {
    // The user's case: a wide main screen directly in front (large angular
    // footprint, samples sprawl across ±18° yaw), and a small laptop
    // down-and-to-the-left. The Gaussian classifier failed this because the
    // main's σ grew to cover the whole screen and its 2σ tail ate the laptop.
    PassiveCalibrator c;
    c.setLayoutPriors({makePrior(M1, 0.0,   0.0, /*hExtYaw=*/20.0, /*hExtPitch=*/10.0),
                       makePrior(M2, -15.0, -10.0, /*hExtYaw=*/5.0,  /*hExtPitch=*/5.0)});

    // 50 main-screen samples sprawled across the wide panel.
    for (int i = 0; i < 50; ++i) {
        const double yaw = -18.0 + (36.0 * i) / 49.0;  // −18° → +18°
        c.recordEvidence(M1, yaw, 0.0, 1.0);
    }
    // Far fewer laptop samples — realistic — clustered tightly at its center.
    for (int i = 0; i < 30; ++i) {
        c.recordEvidence(M2, -15.0, -10.0, 1.0);
    }

    auto laptop = c.classify(-15.0, -10.0, 1.0);
    REQUIRE(laptop.has_value()); REQUIRE(*laptop == M2);

    auto center = c.classify(0.0, 0.0, 1.0);
    REQUIRE(center.has_value()); REQUIRE(*center == M1);
}

TEST_CASE("layout prior alone biases toward the geometrically-nearest screen",
          "[calibrator]") {
    // Before any real evidence, classify() should still refuse (cold start),
    // but ClassifyDetail.best — the argmax regardless of margin — should
    // pick the screen whose layout prior is closest. This is the seed for
    // the live-feedback bar chart.
    PassiveCalibrator c;
    c.setLayoutPriors({makePrior(M1, -20.0, 0.0), makePrior(M2, 20.0, 0.0)});
    const auto d = c.classifyDetailed(-19.0, 0.0, 1.0);
    REQUIRE(d.best == M1);
    REQUIRE_FALSE(d.committed);   // still cold
}

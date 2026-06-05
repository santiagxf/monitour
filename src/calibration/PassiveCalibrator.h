#pragma once
#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace monitour::calibration {

// Per-monitor Gaussian over (yaw, pitch). Updated only on high-confidence
// cursor evidence (cursor parked + keystroke/click on that monitor + face
// confident).
//
// Classification: argmax N(yaw,pitch | μᵢ, Σᵢ) (diagonal Σ), suppressed when the
// second-best monitor's likelihood is within `ambiguityMargin` (log-ratio) OR
// when the observation is an outlier to even the best monitor (more than
// `maxGatingMahalanobis` std-devs away). Modelling pitch as well as yaw means
// the discriminating axis is learned, not assumed: side-by-side screens are
// separated by yaw, vertically-stacked screens by pitch, and "looking at the
// keyboard / away from every screen" is rejected by the outlier gate regardless
// of arrangement — with no hardcoded angle limit.
//
// Until every monitor has nᵢ ≥ minSamples AND the closest pair of means is at
// least `minMeanSeparationDeg` apart (combined yaw+pitch distance),
// classification refuses → no switch.
//
// Persisted to JSON at %LOCALAPPDATA%\Monitour\calibration.json.
class PassiveCalibrator {
public:
    struct Stats {
        double yawMean{0.0};
        double yawVar{900.0};     // ~30° initial stddev
        double pitchMean{0.0};
        double pitchVar{900.0};
        uint64_t n{0};
    };

    // A soft prior for one monitor derived from its physical screen position
    // (Windows already knows the arrangement). yawDeg/pitchDeg are the expected
    // head pose when facing that screen: rightmost screen → +yaw, topmost
    // screen → +pitch (looking up).
    struct LayoutHint {
        HMONITOR monitor{nullptr};
        double   yawDeg{0.0};
        double   pitchDeg{0.0};
    };

    struct Config {
        double  emaAlpha             = 0.05;
        uint64_t minSamples          = 30;
        double  minMeanSeparationDeg = 8.0;
        double  ambiguityMarginNats  = 0.7;   // log-ratio threshold
        double  minFaceConfidence    = 0.6;
        // Reject a classification when the observed (yaw,pitch) is farther than
        // this many standard deviations (combined, diagonal Mahalanobis) from
        // even the best-matching monitor. This is the data-driven replacement
        // for a hardcoded pitch limit: looking at the keyboard / away from every
        // screen lands outside all learned ranges and is ignored, no matter how
        // the monitors are physically arranged (side-by-side or stacked).
        double  maxGatingMahalanobis = 4.0;
        std::chrono::milliseconds cursorParkedMin{500};
    };

    PassiveCalibrator() = default;
    explicit PassiveCalibrator(Config cfg) : cfg_{cfg} {}

    // High-confidence evidence sample: when the gating preconditions hold,
    // the orchestrator calls this. We update only μ,σ (yaw and pitch) for
    // `monitor`. Returns true if the sample was accepted, false if it was
    // dropped (no monitor or face confidence below minFaceConfidence) — the
    // caller uses this to tell the user *why* learning is or isn't progressing.
    bool recordEvidence(HMONITOR monitor, double yawDeg, double pitchDeg,
                        double faceConfidence);

    // Seeds soft (yaw,pitch) priors from the physical monitor arrangement
    // (Windows already knows it). Each hint carries the expected head pose when
    // facing that screen. This only sets a single pseudo-count prior per monitor
    // that has no real data yet — it does NOT bias classification (which still
    // refuses until matured on real evidence) and does NOT advance maturity (one
    // sample ≪ minSamples). Existing learned statistics are left untouched.
    void seedLayout(const std::vector<LayoutHint>& hints);

    // Classify a (yaw,pitch) observation. Returns the monitor when confident,
    // std::nullopt otherwise (cold start, ambiguous, outlier, or low
    // confidence).
    std::optional<HMONITOR> classify(double yawDeg, double pitchDeg,
                                     double faceConfidence) const;

    // True once the calibrator has learned enough to start switching focus
    // (every monitor has ≥ minSamples AND the closest pair of means is
    // ≥ minMeanSeparationDeg apart). This is the passive→active transition.
    bool isMature() const;

    // Quantifies how close we are to maturing (passive→active). `overall` is a
    // 0..1 fraction that reaches 1.0 exactly when isMature() becomes true; the
    // other fields expose the underlying gates for diagnostics / UI.
    struct MaturityProgress {
        double   overall{0.0};             // 0..1 progress toward active mode
        bool     mature{false};
        size_t   monitorsSeen{0};
        uint64_t minSampleCount{0};        // samples on the weaker leading monitor
        uint64_t minSamples{0};            // per-monitor sample target
        double   meanSeparationDeg{0.0};   // current closest-pair separation
        double   minMeanSeparationDeg{0.0};// required separation
    };
    MaturityProgress maturityProgress() const;

    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    // Returns a snapshot (for diagnostics / tests).
    std::unordered_map<HMONITOR, Stats> snapshot() const;

    const Config& config() const noexcept { return cfg_; }

private:
    bool isMature_unlocked() const;

    mutable std::mutex                                mutex_;
    std::unordered_map<HMONITOR, Stats>               stats_;
    Config                                            cfg_{};
};

}  // namespace monitour::calibration

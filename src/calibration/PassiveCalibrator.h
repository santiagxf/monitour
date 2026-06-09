#pragma once
#include <Windows.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace monitour::calibration {

// Per-monitor 2-D histogram over (yaw, pitch), combined with a permanent
// geometric layout prior derived from each screen's physical position and
// angular footprint.
//
// Why the histogram instead of a single Gaussian? A wide screen directly in
// front of the user has a wide angular footprint — yaw samples for it span
// many degrees. A single Gaussian fit to those samples grows a large σ and its
// 2σ tail eats every adjacent screen, including a small laptop angled
// slightly to the side. The histogram represents each screen's true
// distribution, so a tight cluster (the laptop) survives next to a broad one
// (the wide screen).
//
// Why the persistent layout prior? Windows already reports the physical
// arrangement. Folding that into classification as a permanent Bayesian term
// (weight = anchor / (anchor + n)) lets the classifier do something sensible
// at cold start (use Windows' geometry) AND give the wide-screen Gaussian a
// matching wide prior versus the laptop's narrow one. The prior decays in
// relative weight as histogram evidence accumulates, but is never erased — so
// the wide-vs-narrow asymmetry stays informative forever.
//
// Persisted to JSON at %LOCALAPPDATA%\Monitour\calibration.json (version 2).
class PassiveCalibrator {
public:
    struct Config {
        // Histogram grid. Covers ±60° yaw / ±40° pitch at 2° resolution
        // (60×40 = 2400 cells per monitor — trivial memory).
        double yawMinDeg          = -60.0;
        double yawMaxDeg          =  60.0;
        double pitchMinDeg        = -40.0;
        double pitchMaxDeg        =  40.0;
        double cellDeg            =  2.0;

        // Per-sample Gaussian splat: each recordEvidence deposits a 5×5
        // (radius 2 cells, σ = 1 cell) Gaussian patch so single samples
        // generalize to neighboring cells without being averaged away.
        int    splatRadiusCells   = 2;
        double splatSigmaCells    = 1.0;

        // Per-monitor sample count needed to mature. The Gaussian-era
        // mean-separation gate is gone — separation only made sense when each
        // screen was a single point estimate.
        uint64_t minSamples       = 30;

        // Margin between best and second-best score (in nats). Tighter than
        // the Gaussian era because the histogram + prior produces sharper
        // distributions.
        double ambiguityMarginNats = 0.4;

        double minFaceConfidence  = 0.6;

        // Weight of the layout prior is anchor / (anchor + n). At n = 0 the
        // prior dominates; at n = priorAnchorSamples the prior and histogram
        // are equal; for n ≫ anchor histogram evidence rules.
        double priorAnchorSamples = 30.0;

        // Reject when the best monitor's (yaw, pitch) is farther than this
        // many σ from its prior's center (Mahalanobis distance in the
        // prior's own σ units). σ = halfExtent/2 puts the screen edge at 2σ,
        // so 4σ ≈ two whole screens away from where that screen actually
        // sits — the observation must be outside every screen's plausible
        // footprint to be rejected. Scale-invariant by construction: a wide
        // screen rejects "looking at the keyboard" the same way a narrow
        // laptop does, just at different absolute angles.
        double maxPriorMahalanobis = 4.0;
    };

    // Per-monitor geometric prior, derived from the physical screen
    // arrangement that Windows reports. yawDeg/pitchDeg is the expected head
    // pose when facing the screen's center; halfExtent*Deg is the angular
    // half-width of the screen at the user's eye (so a wide screen close to
    // the user gets a wide σ, and a small distant laptop a tight σ).
    struct LayoutPrior {
        HMONITOR monitor{nullptr};
        double   yawDeg{0.0};
        double   pitchDeg{0.0};
        double   halfExtentYawDeg{15.0};
        double   halfExtentPitchDeg{10.0};
    };

    struct ClassifyDetail {
        struct PerMonitor {
            HMONITOR monitor{nullptr};
            double   logScore{0.0};   // log_hist + w_prior * log_prior
            double   logHist{0.0};
            double   logPrior{0.0};
            uint64_t samples{0};
        };
        std::vector<PerMonitor> scores;       // one entry per known monitor
        HMONITOR best{nullptr};               // argmax (regardless of margin)
        HMONITOR second{nullptr};
        bool     committed{false};            // would classify() return non-null?
    };

    PassiveCalibrator() = default;
    explicit PassiveCalibrator(Config cfg) : cfg_{cfg} {}

    // High-confidence evidence sample. The caller has already verified the
    // input gating (cursor parked, real click/keystroke, face confident).
    // Returns true if the sample was accepted, false if it was dropped
    // (unknown monitor or low face confidence).
    bool recordEvidence(HMONITOR monitor, double yawDeg, double pitchDeg,
                        double faceConfidence);

    // Install the geometric layout priors. Idempotent (overwrites existing
    // priors and ensures one histogram slot exists per known monitor without
    // touching accumulated cells). Call once at startup and again on display
    // change.
    void setLayoutPriors(const std::vector<LayoutPrior>& priors);

    // Classify a (yaw,pitch) observation. Returns the monitor when confident,
    // std::nullopt otherwise (cold start with < 2 monitors known, ambiguous,
    // outlier to every prior, or low face confidence).
    std::optional<HMONITOR> classify(double yawDeg, double pitchDeg,
                                     double faceConfidence) const;

    // Same as classify() but returns the per-monitor scores so the debug
    // overlay can show *why* the classifier is committing or refusing.
    ClassifyDetail classifyDetailed(double yawDeg, double pitchDeg,
                                    double faceConfidence) const;

    // True once every known monitor has ≥ minSamples real evidence.
    bool isMature() const;

    struct MaturityProgress {
        double   overall{0.0};      // 0..1
        bool     mature{false};
        size_t   monitorsSeen{0};
        uint64_t minSampleCount{0}; // sample count on the weakest monitor
        uint64_t minSamples{0};     // per-monitor target
    };
    MaturityProgress maturityProgress() const;

    // Fraction of histogram mass that classifies back to its own monitor —
    // walks every cell × every monitor. Only invoke on a slow cadence (the
    // debug overlay does so ~1 Hz) — too heavy to put in
    // `maturityProgress()`, which the worker calls per inference tick.
    double computeSelfAccuracy() const;

    bool load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    struct Snapshot {
        uint64_t samples{0};
        std::vector<double> cells;        // yawCells * pitchCells
        LayoutPrior         prior{};
    };
    std::unordered_map<HMONITOR, Snapshot> snapshot() const;

    const Config& config() const noexcept { return cfg_; }

    int yawCells() const noexcept;
    int pitchCells() const noexcept;

private:
    struct MonitorState {
        std::vector<double> cells;        // yawCells * pitchCells, row-major
        uint64_t            samples{0};
        LayoutPrior         prior{};
    };

    bool isMature_unlocked() const;
    void ensureCells_unlocked(MonitorState& s) const;

    // Bilinear lookup into the histogram in degrees → log density.
    double logHistAt_unlocked(const MonitorState& s,
                              double yawDeg, double pitchDeg) const;
    // Per-monitor layout prior log density at (yaw, pitch).
    double logPriorAt(const LayoutPrior& p,
                      double yawDeg, double pitchDeg) const;
    // Scale-invariant Mahalanobis distance from (yaw, pitch) to the prior's
    // center, in σ units (so 2 = screen edge regardless of screen size).
    double priorMahalanobis(const LayoutPrior& p,
                            double yawDeg, double pitchDeg) const;
    // Score (combined) at (yaw, pitch) given anchor weighting.
    double scoreAt_unlocked(const MonitorState& s,
                            double yawDeg, double pitchDeg,
                            double& outLogHist, double& outLogPrior) const;
    double selfAccuracy_unlocked() const;

    mutable std::mutex                              mutex_;
    std::unordered_map<HMONITOR, MonitorState>      states_;
    Config                                          cfg_{};
};

}  // namespace monitour::calibration

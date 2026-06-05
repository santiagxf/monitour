#include "PassiveCalibrator.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <vector>

#include "util/Logging.h"

namespace monitour::calibration {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kMinVariance = 4.0;   // floor σ at 2°

double logPdf(double x, double mean, double variance) {
    double v = std::max(variance, kMinVariance);
    double d = x - mean;
    return -0.5 * (std::log(kTwoPi * v) + (d * d) / v);
}

// Distance between two monitor means in the combined (yaw,pitch) plane. Using
// the Euclidean distance means either axis can provide the separation:
// side-by-side screens separate in yaw, vertically-stacked screens in pitch.
double meanDistance(const PassiveCalibrator::Stats& a,
                    const PassiveCalibrator::Stats& b) {
    const double dy = a.yawMean - b.yawMean;
    const double dp = a.pitchMean - b.pitchMean;
    return std::sqrt(dy * dy + dp * dp);
}

}  // namespace

bool PassiveCalibrator::recordEvidence(HMONITOR monitor, double yawDeg,
                                       double pitchDeg, double faceConfidence) {
    if (!monitor) return false;
    if (faceConfidence < cfg_.minFaceConfidence) return false;

    std::lock_guard lock{mutex_};
    auto& s = stats_[monitor];
    if (s.n == 0) {
        s.yawMean = yawDeg;
        s.yawVar = 9.0;     // start with ~3° stddev once we have any data
        s.pitchMean = pitchDeg;
        s.pitchVar = 9.0;
        s.n = 1;
        return true;
    }
    // EMA update for mean and variance (Welford-style with fixed α), per axis.
    auto emaUpdate = [a = cfg_.emaAlpha](double& mean, double& var, double x) {
        double oldMean = mean;
        mean = (1.0 - a) * mean + a * x;
        double dev = x - oldMean;
        var = (1.0 - a) * var + a * (dev * dev);
        if (var < kMinVariance) var = kMinVariance;
    };
    emaUpdate(s.yawMean, s.yawVar, yawDeg);
    emaUpdate(s.pitchMean, s.pitchVar, pitchDeg);
    s.n += 1;
    return true;
}

void PassiveCalibrator::seedLayout(const std::vector<LayoutHint>& hints) {
    std::lock_guard lock{mutex_};
    if (hints.empty()) return;

    for (const auto& h : hints) {
        if (!h.monitor) continue;

        // Don't clobber monitors that already carry real (or loaded) evidence.
        auto it = stats_.find(h.monitor);
        if (it != stats_.end() && it->second.n > 1) continue;

        Stats s{};
        s.yawMean   = h.yawDeg;
        s.pitchMean = h.pitchDeg;
        s.yawVar    = 225.0;  // wide ~15° prior; real evidence tightens it
        s.pitchVar  = 225.0;
        s.n         = 1;      // single pseudo-count: a soft prior, far below minSamples
        stats_[h.monitor] = s;
    }
}

bool PassiveCalibrator::isMature_unlocked() const {
    if (stats_.size() < 2) return false;

    double closestSep = std::numeric_limits<double>::infinity();
    for (auto a = stats_.begin(); a != stats_.end(); ++a) {
        if (a->second.n < cfg_.minSamples) return false;
        for (auto b = std::next(a); b != stats_.end(); ++b) {
            closestSep = std::min(closestSep, meanDistance(a->second, b->second));
        }
    }
    return closestSep >= cfg_.minMeanSeparationDeg;
}

bool PassiveCalibrator::isMature() const {
    std::lock_guard lock{mutex_};
    return isMature_unlocked();
}

PassiveCalibrator::MaturityProgress PassiveCalibrator::maturityProgress() const {
    std::lock_guard lock{mutex_};

    MaturityProgress p{};
    p.minSamples           = cfg_.minSamples;
    p.minMeanSeparationDeg = cfg_.minMeanSeparationDeg;
    p.monitorsSeen         = stats_.size();
    p.mature               = isMature_unlocked();

    // Sample factor: the two best-populated monitors must each reach
    // minSamples. Take the top-two sample counts (missing monitor counts as 0).
    std::vector<uint64_t> counts;
    counts.reserve(stats_.size());
    for (auto& [mon, s] : stats_) counts.push_back(s.n);
    std::sort(counts.begin(), counts.end(), std::greater<>{});

    auto sampleFrac = [&](size_t i) -> double {
        const uint64_t n = i < counts.size() ? counts[i] : 0;
        if (cfg_.minSamples == 0) return 1.0;
        const double f = static_cast<double>(n) / static_cast<double>(cfg_.minSamples);
        return f > 1.0 ? 1.0 : f;
    };
    const double sampleFactor = 0.5 * (sampleFrac(0) + sampleFrac(1));
    p.minSampleCount = counts.size() >= 2 ? counts[1] : 0;

    // Separation factor: closest pair of means vs. the required threshold.
    double closestSep = std::numeric_limits<double>::infinity();
    for (auto a = stats_.begin(); a != stats_.end(); ++a) {
        for (auto b = std::next(a); b != stats_.end(); ++b) {
            closestSep = std::min(closestSep, meanDistance(a->second, b->second));
        }
    }
    double sepFactor = 0.0;
    if (stats_.size() >= 2 && std::isfinite(closestSep)) {
        p.meanSeparationDeg = closestSep;
        sepFactor = cfg_.minMeanSeparationDeg > 0.0
                        ? closestSep / cfg_.minMeanSeparationDeg
                        : 1.0;
        if (sepFactor > 1.0) sepFactor = 1.0;
    }

    // Overall progress is multiplicative: maturity requires BOTH enough samples
    // AND separated means, so neither gate alone should inflate the number. In
    // particular, layout seeding pre-separates the means, so an additive form
    // would report ~50% before any real evidence exists. Multiplying keeps the
    // bar near 0 until real samples accumulate.
    double overall = sampleFactor * sepFactor;
    if (p.mature) {
        overall = 1.0;
    } else if (overall > 0.999) {
        overall = 0.999;  // not mature yet — never report a full 100%
    }
    p.overall = overall;
    return p;
}

std::optional<HMONITOR> PassiveCalibrator::classify(double yawDeg,
                                                    double pitchDeg,
                                                    double faceConfidence) const {
    if (faceConfidence < cfg_.minFaceConfidence) return std::nullopt;
    std::lock_guard lock{mutex_};
    if (!isMature_unlocked()) return std::nullopt;

    HMONITOR bestMon = nullptr;
    const Stats* bestStats = nullptr;
    double bestLp = -std::numeric_limits<double>::infinity();
    double secondLp = -std::numeric_limits<double>::infinity();

    for (auto& [mon, s] : stats_) {
        // Joint log-likelihood over independent yaw and pitch Gaussians.
        double lp = logPdf(yawDeg, s.yawMean, s.yawVar) +
                    logPdf(pitchDeg, s.pitchMean, s.pitchVar);
        if (lp > bestLp) {
            secondLp = bestLp;
            bestLp = lp;
            bestMon = mon;
            bestStats = &s;
        } else if (lp > secondLp) {
            secondLp = lp;
        }
    }

    if (!bestMon || !bestStats) return std::nullopt;

    // Outlier gate (data-driven replacement for a hardcoded pitch limit): if the
    // observation is too far from even the best monitor's learned (yaw,pitch)
    // range, the user is looking somewhere we've never learned — the keyboard,
    // a phone, away from every screen. Refuse rather than guess. This adapts to
    // whatever arrangement the user actually has, including stacked monitors.
    const double dyaw = yawDeg - bestStats->yawMean;
    const double dpitch = pitchDeg - bestStats->pitchMean;
    const double maha2 = (dyaw * dyaw) / std::max(bestStats->yawVar, kMinVariance) +
                         (dpitch * dpitch) / std::max(bestStats->pitchVar, kMinVariance);
    const double gate = cfg_.maxGatingMahalanobis * cfg_.maxGatingMahalanobis;
    if (maha2 > gate) return std::nullopt;

    if ((bestLp - secondLp) >= cfg_.ambiguityMarginNats) {
        return bestMon;
    }
    return std::nullopt;
}

std::unordered_map<HMONITOR, PassiveCalibrator::Stats>
PassiveCalibrator::snapshot() const {
    std::lock_guard lock{mutex_};
    return stats_;
}

// --- JSON persistence (minimal, no external library) ---

bool PassiveCalibrator::save(const std::filesystem::path& path) const {
    std::lock_guard lock{mutex_};
    std::ofstream f{path};
    if (!f) return false;
    f << "{\n  \"monitors\": [\n";
    bool first = true;
    for (auto& [mon, s] : stats_) {
        if (!first) f << ",\n";
        first = false;
        // We persist by HMONITOR's bit pattern; on next launch the HMONITOR
        // values may differ, so during load we re-key by device-rect identity.
        // For now we round-trip the bit pattern as a debugging aid; the real
        // re-key step is a TODO in load().
        f << "    {\"hmon\": " << reinterpret_cast<uintptr_t>(mon)
          << ", \"yawMean\": " << s.yawMean
          << ", \"yawVar\": " << s.yawVar
          << ", \"pitchMean\": " << s.pitchMean
          << ", \"pitchVar\": " << s.pitchVar
          << ", \"n\": " << s.n
          << "}";
    }
    f << "\n  ]\n}\n";
    return f.good();
}

bool PassiveCalibrator::load(const std::filesystem::path& path) {
    std::ifstream f{path};
    if (!f) return false;
    // TODO: replace with a JSON parser pass; re-key by current monitor topology
    // (DEVMODE / EnumDisplayDevices) instead of raw HMONITOR.
    log::warn(L"PassiveCalibrator::load: JSON parser not yet implemented.");
    return false;
}

}  // namespace monitour::calibration

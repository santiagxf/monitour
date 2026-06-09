#include "PassiveCalibrator.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#include "util/Logging.h"

namespace monitour::calibration {

namespace {

constexpr double kEpsilon = 1e-9;
constexpr double kMinHalfExtentDeg = 3.0;   // tighter prior than this would be brittle

int cellIndex(double deg, double minDeg, double cellDeg, int cells) {
    int idx = static_cast<int>(std::floor((deg - minDeg) / cellDeg));
    if (idx < 0) idx = 0;
    if (idx >= cells) idx = cells - 1;
    return idx;
}

}  // namespace

int PassiveCalibrator::yawCells() const noexcept {
    return std::max(
        1,
        static_cast<int>(std::lround((cfg_.yawMaxDeg - cfg_.yawMinDeg) / cfg_.cellDeg)));
}

int PassiveCalibrator::pitchCells() const noexcept {
    return std::max(
        1,
        static_cast<int>(std::lround((cfg_.pitchMaxDeg - cfg_.pitchMinDeg) / cfg_.cellDeg)));
}

void PassiveCalibrator::ensureCells_unlocked(MonitorState& s) const {
    const size_t n = static_cast<size_t>(yawCells()) *
                     static_cast<size_t>(pitchCells());
    if (s.cells.size() != n) s.cells.assign(n, 0.0);
}

bool PassiveCalibrator::recordEvidence(HMONITOR monitor, double yawDeg,
                                       double pitchDeg, double faceConfidence) {
    if (!monitor) return false;
    if (faceConfidence < cfg_.minFaceConfidence) return false;

    std::lock_guard lock{mutex_};
    auto& s = states_[monitor];
    ensureCells_unlocked(s);

    const int yCells = yawCells();
    const int pCells = pitchCells();
    const int yc = cellIndex(yawDeg, cfg_.yawMinDeg, cfg_.cellDeg, yCells);
    const int pc = cellIndex(pitchDeg, cfg_.pitchMinDeg, cfg_.cellDeg, pCells);

    // Gaussian splat in a (2r+1) × (2r+1) neighborhood around (yc, pc). The
    // patch integrates to ~1.0 so one sample is one sample, but its weight is
    // distributed across nearby cells — a single click no longer pins a sharp
    // delta into one cell, which would lose to a neighboring cell on a
    // millimetre of head jitter.
    const int   r     = cfg_.splatRadiusCells;
    const double sigma = std::max(cfg_.splatSigmaCells, 0.25);
    double total = 0.0;
    for (int dp = -r; dp <= r; ++dp) {
        for (int dy = -r; dy <= r; ++dy) {
            total += std::exp(-(dy * dy + dp * dp) / (2.0 * sigma * sigma));
        }
    }
    if (total < kEpsilon) total = 1.0;

    for (int dp = -r; dp <= r; ++dp) {
        const int py = pc + dp;
        if (py < 0 || py >= pCells) continue;
        for (int dy = -r; dy <= r; ++dy) {
            const int yx = yc + dy;
            if (yx < 0 || yx >= yCells) continue;
            const double w =
                std::exp(-(dy * dy + dp * dp) / (2.0 * sigma * sigma)) / total;
            s.cells[static_cast<size_t>(py) * yCells + yx] += w;
        }
    }
    s.samples += 1;
    return true;
}

void PassiveCalibrator::setLayoutPriors(const std::vector<LayoutPrior>& priors) {
    std::lock_guard lock{mutex_};
    for (const auto& p : priors) {
        if (!p.monitor) continue;
        auto& s = states_[p.monitor];
        ensureCells_unlocked(s);
        s.prior = p;
        s.prior.halfExtentYawDeg =
            std::max(s.prior.halfExtentYawDeg, kMinHalfExtentDeg);
        s.prior.halfExtentPitchDeg =
            std::max(s.prior.halfExtentPitchDeg, kMinHalfExtentDeg);
    }
}

double PassiveCalibrator::logHistAt_unlocked(const MonitorState& s,
                                             double yawDeg,
                                             double pitchDeg) const {
    if (s.cells.empty() || s.samples == 0) {
        // Uniform-ish fallback when this monitor has no real data.
        return -std::log(static_cast<double>(yawCells() * pitchCells()));
    }
    const int yCells = yawCells();
    const int pCells = pitchCells();

    const double fy = (yawDeg - cfg_.yawMinDeg) / cfg_.cellDeg - 0.5;
    const double fp = (pitchDeg - cfg_.pitchMinDeg) / cfg_.cellDeg - 0.5;
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, yCells - 1);
    const int p0 = std::clamp(static_cast<int>(std::floor(fp)), 0, pCells - 1);
    const int y1 = std::clamp(y0 + 1, 0, yCells - 1);
    const int p1 = std::clamp(p0 + 1, 0, pCells - 1);
    const double ty = std::clamp(fy - y0, 0.0, 1.0);
    const double tp = std::clamp(fp - p0, 0.0, 1.0);

    auto at = [&](int yc, int pc) {
        return s.cells[static_cast<size_t>(pc) * yCells + yc];
    };
    const double v00 = at(y0, p0);
    const double v10 = at(y1, p0);
    const double v01 = at(y0, p1);
    const double v11 = at(y1, p1);
    const double v =
        (1.0 - ty) * (1.0 - tp) * v00 + ty * (1.0 - tp) * v10 +
        (1.0 - ty) * tp        * v01 + ty * tp        * v11;

    // Add-one smoothing so a brand-new cell isn't −infinity, and normalize by
    // total mass so the per-monitor histogram is a proper probability density
    // (in cell units). The −log(area) term converts that to a per-degree
    // density.
    const double mass = static_cast<double>(s.samples);  // splats sum to ~1
    const double smoothed = (v + 1.0 / (yCells * pCells)) / (mass + 1.0);
    const double cellArea = cfg_.cellDeg * cfg_.cellDeg;
    return std::log(std::max(smoothed, kEpsilon)) - std::log(cellArea);
}

double PassiveCalibrator::logPriorAt(const LayoutPrior& p,
                                     double yawDeg, double pitchDeg) const {
    // Independent Gaussian over yaw and pitch with σ = halfExtent / 2 (so the
    // screen edge sits at ~2σ). Constant terms are kept so the absolute value
    // is comparable across monitors with different extents.
    const double sigY = std::max(p.halfExtentYawDeg, kMinHalfExtentDeg) / 2.0;
    const double sigP = std::max(p.halfExtentPitchDeg, kMinHalfExtentDeg) / 2.0;
    const double dy = (yawDeg - p.yawDeg) / sigY;
    const double dp = (pitchDeg - p.pitchDeg) / sigP;
    constexpr double kLog2Pi = 1.8378770664093453;
    return -0.5 * (dy * dy + dp * dp) - kLog2Pi -
           std::log(sigY) - std::log(sigP);
}

double PassiveCalibrator::priorMahalanobis(const LayoutPrior& p,
                                           double yawDeg, double pitchDeg) const {
    // Mahalanobis distance in the prior's own σ units (σ = halfExtent / 2).
    // Scale-invariant: the screen edge is always at distance 2, regardless
    // of whether the screen is a tiny laptop or a 49" ultrawide.
    const double sigY = std::max(p.halfExtentYawDeg, kMinHalfExtentDeg) / 2.0;
    const double sigP = std::max(p.halfExtentPitchDeg, kMinHalfExtentDeg) / 2.0;
    const double dy = (yawDeg - p.yawDeg) / sigY;
    const double dp = (pitchDeg - p.pitchDeg) / sigP;
    return std::sqrt(dy * dy + dp * dp);
}

double PassiveCalibrator::scoreAt_unlocked(const MonitorState& s,
                                           double yawDeg, double pitchDeg,
                                           double& outLogHist,
                                           double& outLogPrior) const {
    outLogHist  = logHistAt_unlocked(s, yawDeg, pitchDeg);
    outLogPrior = logPriorAt(s.prior, yawDeg, pitchDeg);
    const double n = static_cast<double>(s.samples);
    const double wPrior =
        cfg_.priorAnchorSamples / (cfg_.priorAnchorSamples + n);
    return outLogHist + wPrior * outLogPrior;
}

PassiveCalibrator::ClassifyDetail
PassiveCalibrator::classifyDetailed(double yawDeg, double pitchDeg,
                                    double faceConfidence) const {
    ClassifyDetail out;
    std::lock_guard lock{mutex_};
    out.scores.reserve(states_.size());

    double bestScore = -std::numeric_limits<double>::infinity();
    double secondScore = -std::numeric_limits<double>::infinity();
    double bestMaha = std::numeric_limits<double>::infinity();

    for (auto& [mon, s] : states_) {
        ClassifyDetail::PerMonitor pm{};
        pm.monitor = mon;
        pm.samples = s.samples;
        pm.logScore = scoreAt_unlocked(s, yawDeg, pitchDeg,
                                       pm.logHist, pm.logPrior);
        out.scores.push_back(pm);

        if (pm.logScore > bestScore) {
            secondScore = bestScore;
            out.second  = out.best;
            bestScore = pm.logScore;
            bestMaha  = priorMahalanobis(s.prior, yawDeg, pitchDeg);
            out.best  = mon;
        } else if (pm.logScore > secondScore) {
            secondScore = pm.logScore;
            out.second  = mon;
        }
    }

    // Scale-invariant outlier gate: the best monitor's (yaw, pitch) must lie
    // within `maxPriorMahalanobis` σ of its prior. σ scales with the screen's
    // angular footprint, so the gate is "two screens away from where the
    // screen is" regardless of whether the screen is a laptop or an
    // ultrawide. A raw log-density floor would reject the edges of any large
    // screen because its constant terms grow with σ — Mahalanobis sidesteps
    // that entirely.
    out.committed =
        faceConfidence >= cfg_.minFaceConfidence &&
        states_.size() >= 2 &&
        isMature_unlocked() &&
        out.best != nullptr &&
        bestMaha <= cfg_.maxPriorMahalanobis &&
        (bestScore - secondScore) >= cfg_.ambiguityMarginNats;
    return out;
}

std::optional<HMONITOR> PassiveCalibrator::classify(double yawDeg,
                                                    double pitchDeg,
                                                    double faceConfidence) const {
    const auto d = classifyDetailed(yawDeg, pitchDeg, faceConfidence);
    if (d.committed) return d.best;
    return std::nullopt;
}

bool PassiveCalibrator::isMature_unlocked() const {
    if (states_.size() < 2) return false;
    for (auto& [mon, s] : states_) {
        if (s.samples < cfg_.minSamples) return false;
    }
    return true;
}

bool PassiveCalibrator::isMature() const {
    std::lock_guard lock{mutex_};
    return isMature_unlocked();
}

double PassiveCalibrator::computeSelfAccuracy() const {
    std::lock_guard lock{mutex_};
    return selfAccuracy_unlocked();
}

double PassiveCalibrator::selfAccuracy_unlocked() const {
    // Walks every monitor's histogram cell, classifies its center under the
    // combined score, and reports the fraction of mass that lands on its own
    // monitor. Heavy (a few thousand score evaluations) — only callable via
    // computeSelfAccuracy() on a slow cadence, not on the worker's hot loop.
    if (states_.size() < 2) return 0.0;
    const int yCells = yawCells();
    const int pCells = pitchCells();
    double totalMass = 0.0;
    double correctMass = 0.0;

    for (auto& [mon, s] : states_) {
        if (s.cells.empty()) continue;
        for (int p = 0; p < pCells; ++p) {
            const double pitch =
                cfg_.pitchMinDeg + (p + 0.5) * cfg_.cellDeg;
            for (int y = 0; y < yCells; ++y) {
                const double mass = s.cells[static_cast<size_t>(p) * yCells + y];
                if (mass <= 0.0) continue;
                const double yaw = cfg_.yawMinDeg + (y + 0.5) * cfg_.cellDeg;

                double bestScore = -std::numeric_limits<double>::infinity();
                HMONITOR bestMon = nullptr;
                for (auto& [m2, s2] : states_) {
                    double lh, lp;
                    const double sc = scoreAt_unlocked(s2, yaw, pitch, lh, lp);
                    if (sc > bestScore) { bestScore = sc; bestMon = m2; }
                }
                totalMass += mass;
                if (bestMon == mon) correctMass += mass;
            }
        }
    }
    if (totalMass < kEpsilon) return 0.0;
    return correctMass / totalMass;
}

PassiveCalibrator::MaturityProgress
PassiveCalibrator::maturityProgress() const {
    std::lock_guard lock{mutex_};
    MaturityProgress p{};
    p.minSamples   = cfg_.minSamples;
    p.monitorsSeen = states_.size();
    p.mature       = isMature_unlocked();

    uint64_t weakest = std::numeric_limits<uint64_t>::max();
    for (auto& [mon, s] : states_) {
        weakest = std::min(weakest, s.samples);
    }
    if (weakest == std::numeric_limits<uint64_t>::max()) weakest = 0;
    p.minSampleCount = weakest;

    const double sampleFrac =
        cfg_.minSamples == 0
            ? 1.0
            : std::min(1.0, static_cast<double>(weakest) /
                                static_cast<double>(cfg_.minSamples));
    const double monitorFrac =
        states_.size() >= 2 ? 1.0 : 0.0;

    double overall = sampleFrac * monitorFrac;
    if (p.mature) {
        overall = 1.0;
    } else if (overall > 0.999) {
        overall = 0.999;
    }
    p.overall = overall;
    return p;
}

std::unordered_map<HMONITOR, PassiveCalibrator::Snapshot>
PassiveCalibrator::snapshot() const {
    std::lock_guard lock{mutex_};
    std::unordered_map<HMONITOR, Snapshot> out;
    out.reserve(states_.size());
    for (auto& [mon, s] : states_) {
        Snapshot snap;
        snap.samples = s.samples;
        snap.cells   = s.cells;
        snap.prior   = s.prior;
        out.emplace(mon, std::move(snap));
    }
    return out;
}

// --- JSON persistence (minimal, no external library) ---

bool PassiveCalibrator::save(const std::filesystem::path& path) const {
    std::lock_guard lock{mutex_};
    std::ofstream f{path};
    if (!f) return false;
    f << "{\n  \"version\": 2,\n  \"monitors\": [\n";
    bool firstMon = true;
    for (auto& [mon, s] : states_) {
        if (!firstMon) f << ",\n";
        firstMon = false;
        // HMONITOR is unstable across launches; we still round-trip the bit
        // pattern as a debugging aid. The real re-key step (by device-rect)
        // remains a load() TODO.
        f << "    {\"hmon\": " << reinterpret_cast<uintptr_t>(mon)
          << ", \"samples\": " << s.samples
          << ", \"prior\": {"
          << "\"yaw\": " << s.prior.yawDeg
          << ", \"pitch\": " << s.prior.pitchDeg
          << ", \"halfExtentYaw\": " << s.prior.halfExtentYawDeg
          << ", \"halfExtentPitch\": " << s.prior.halfExtentPitchDeg
          << "}, \"cells\": [";
        for (size_t i = 0; i < s.cells.size(); ++i) {
            if (i) f << ",";
            f << s.cells[i];
        }
        f << "]}";
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

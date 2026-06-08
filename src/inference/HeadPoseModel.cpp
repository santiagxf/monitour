#include "HeadPoseModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "d3d/D3DContext.h"
#include "util/ComUtil.h"
#include "util/Logging.h"

namespace monitour::inference {

namespace {

std::wstring toWide(std::string_view s) {
    return std::wstring(s.begin(), s.end());
}

// 6DRepNet outputs a 3x3 rotation matrix. Decode to Tait-Bryan Euler angles
// (x=pitch, y=yaw, z=roll), matching upstream
// compute_euler_angles_from_rotation_matrices, then to degrees.
void decodeEuler(const float R[9], float& yawDeg, float& pitchDeg,
                 float& rollDeg) {
    const float r00 = R[0], r10 = R[3], r20 = R[6];
    const float r21 = R[7], r22 = R[8];

    const float sy = std::sqrt(r00 * r00 + r10 * r10);
    constexpr float kRad2Deg = 57.29577951308232f;

    float pitch, yaw, roll;
    if (sy > 1e-6f) {
        pitch = std::atan2(r21, r22);
        yaw   = std::atan2(-r20, sy);
        roll  = std::atan2(r10, r00);
    } else {
        pitch = std::atan2(-r21, r22);
        yaw   = std::atan2(-r20, sy);
        roll  = 0.0f;
    }
    pitchDeg = pitch * kRad2Deg;
    yawDeg   = yaw * kRad2Deg;
    rollDeg  = roll * kRad2Deg;
}

}  // namespace

HeadPoseModel::HeadPoseModel(d3d::D3DContext& d3d,
                             const std::filesystem::path& modelPath,
                             DeviceChoice choice,
                             const std::filesystem::path& cacheDir,
                             UINT inputSize)
    : d3d_{d3d}, inputSize_{inputSize} {
    auto plan = makeSessionPlan(choice, cacheDir);
    resolved_ = plan.resolved;

    session_ = std::make_unique<Ort::Session>(
        ortEnv(), modelPath.wstring().c_str(), *plan.options);

    Ort::AllocatorWithDefaultOptions alloc;
    inputName_  = std::string{session_->GetInputNameAllocated(0, alloc).get()};
    outputName_ = std::string{session_->GetOutputNameAllocated(0, alloc).get()};

    log::info(L"HeadPoseModel: loaded {} (in='{}', out='{}')",
              modelPath.wstring(), toWide(inputName_), toWide(outputName_));
    log::info(L"HeadPoseModel: running on {} on {} ({})",
              resolved_.epName, resolved_.deviceType, resolved_.vendor);

    // CPU staging mirror of the preprocessor output. The shader writes
    // RWStructuredBuffer<min16float> with a 4-byte stride (fxc without
    // native 16-bit types backs min16float in a full float32 slot), so each
    // element is a plain float32. Mirror that byte size.
    const UINT elements = 3u * inputSize_ * inputSize_;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth      = elements * sizeof(uint32_t);
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    MONITOUR_CHECK_HR(d3d_.device()->CreateBuffer(&sd, nullptr, staging_.put()));
    inputData_.resize(elements);
}

HeadPoseModel::~HeadPoseModel() = default;

HeadPose HeadPoseModel::evaluate(ID3D11Buffer* preprocessed) {
    HeadPose pose{};
    if (!preprocessed || !session_) return pose;

    auto* ctx = d3d_.context();
    const UINT elements = 3u * inputSize_ * inputSize_;

    // 1. Copy GPU tensor → CPU staging, map, read float32 directly.
    //    Zero-copy via the OpenVINO Remote Tensor API is a follow-up; for
    //    now the readback cost is dwarfed by face detection + inference.
    ctx->CopyResource(staging_.get(), preprocessed);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        log::warn(L"HeadPoseModel: staging Map failed.");
        return pose;
    }
    std::memcpy(inputData_.data(), mapped.pData, elements * sizeof(float));
    ctx->Unmap(staging_.get(), 0);

    // 2. Build the input tensor, run inference, decode the rotation matrix.
    try {
        const std::array<int64_t, 4> shape{
            1, 3, static_cast<int64_t>(inputSize_),
            static_cast<int64_t>(inputSize_)};
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
            memInfo, inputData_.data(), inputData_.size(),
            shape.data(), shape.size());

        const char* inputNames[]  = {inputName_.c_str()};
        const char* outputNames[] = {outputName_.c_str()};

        auto outputs = session_->Run(Ort::RunOptions{nullptr},
                                     inputNames, &input, 1,
                                     outputNames, 1);
        if (outputs.empty()) {
            log::warn(L"HeadPoseModel: empty Run output.");
            return pose;
        }
        const float* R = outputs[0].GetTensorData<float>();
        auto info = outputs[0].GetTensorTypeAndShapeInfo();
        if (info.GetElementCount() < 9) {
            log::warn(L"HeadPoseModel: unexpected output element count {}.",
                      info.GetElementCount());
            return pose;
        }
        decodeEuler(R, pose.yaw, pose.pitch, pose.roll);

        // Plausibility gate (no face-detector score plumbed in here):
        // NaN / out-of-range angles report zero confidence so the
        // calibrator ignores them.
        const bool plausible = std::isfinite(pose.yaw) &&
                               std::isfinite(pose.pitch) &&
                               std::abs(pose.yaw)   <= 90.0f &&
                               std::abs(pose.pitch) <= 90.0f;
        pose.confidence = plausible ? 1.0f : 0.0f;
    } catch (Ort::Exception const& e) {
        log::warn(L"HeadPoseModel: Run failed: {}", toWide(e.what()));
        pose.confidence = 0.0f;
    }

    // Periodic pose-distribution diagnostic. If the model is reliably
    // crashing the plausibility gate, that's why focus-switching never
    // arms — even with a face crop.
    ++evalCount_;
    if (pose.confidence > 0.f) {
        ++plausibleCount_;
        yawMin_   = std::min(yawMin_, pose.yaw);
        yawMax_   = std::max(yawMax_, pose.yaw);
        pitchMin_ = std::min(pitchMin_, pose.pitch);
        pitchMax_ = std::max(pitchMax_, pose.pitch);
    }
    if (evalCount_ % 50 == 0) {
        log::info(L"HeadPose: {} evals, {} plausible "
                  L"(yaw {:.1f}..{:.1f}, pitch {:.1f}..{:.1f}); last "
                  L"yaw={:.1f} pitch={:.1f} roll={:.1f} conf={:.2f}",
                  evalCount_, plausibleCount_,
                  yawMin_, yawMax_, pitchMin_, pitchMax_,
                  pose.yaw, pose.pitch, pose.roll, pose.confidence);
        yawMin_ = pitchMin_ = std::numeric_limits<float>::infinity();
        yawMax_ = pitchMax_ = -std::numeric_limits<float>::infinity();
    }
    return pose;
}

}  // namespace monitour::inference

#include "HeadPoseModel.h"

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "d3d/D3DContext.h"
#include "util/ComUtil.h"
#include "util/Logging.h"

namespace monitour::inference {

using namespace winrt::Windows::AI::MachineLearning;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage;

HeadPoseModel::HeadPoseModel(d3d::D3DContext& d3d,
                             const std::filesystem::path& modelPath,
                             DeviceSelection device,
                             UINT inputSize)
    : d3d_{d3d}, device_{std::move(device)}, inputSize_{inputSize} {
    auto file = StorageFile::GetFileFromPathAsync(modelPath.wstring()).get();
    model_ = LearningModel::LoadFromStorageFileAsync(file).get();
    log::info(L"HeadPoseModel: loaded {} ({} input features, {} output features)",
              modelPath.wstring(),
              static_cast<unsigned>(model_.InputFeatures().Size()),
              static_cast<unsigned>(model_.OutputFeatures().Size()));

    if (model_.InputFeatures().Size() > 0) {
        inputName_ = std::wstring{model_.InputFeatures().GetAt(0).Name()};
    } else {
        inputName_ = L"input";
    }
    if (model_.OutputFeatures().Size() > 0) {
        outputName_ = std::wstring{model_.OutputFeatures().GetAt(0).Name()};
    } else {
        outputName_ = L"output";
    }

    LearningModelSessionOptions opts{};
    // Batch fixed at 1 so the runtime can pre-plan.
    opts.BatchSizeOverride(1);
    session_ = LearningModelSession{model_, device_.device, opts};
    binding_ = LearningModelBinding{session_};

    // Staging buffer for CPU readback of the preprocessed tensor. The compute
    // shader writes a structured buffer of min16float backed by 32-bit slots
    // (4 bytes/element), so mirror that byte size exactly.
    const UINT elements = 3u * inputSize_ * inputSize_;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth      = elements * sizeof(uint32_t);
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    MONITOUR_CHECK_HR(d3d_.device()->CreateBuffer(&sd, nullptr, staging_.put()));
    inputData_.resize(elements);
}

HeadPoseModel::~HeadPoseModel() = default;

namespace {

// 6DRepNet returns a 3x3 rotation matrix; decode it to Euler angles using the
// same convention as the upstream compute_euler_angles_from_rotation_matrices
// (Tait–Bryan, x=pitch, y=yaw, z=roll), then convert to degrees.
void decodeEuler(const float R[9], float& yawDeg, float& pitchDeg,
                 float& rollDeg) {
    // Row-major: R[3*r + c].
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
        // Gimbal lock: roll is undefined; fold it into pitch.
        pitch = std::atan2(-r21, r22);
        yaw   = std::atan2(-r20, sy);
        roll  = 0.0f;
    }
    pitchDeg = pitch * kRad2Deg;
    yawDeg   = yaw * kRad2Deg;
    rollDeg  = roll * kRad2Deg;
}

}  // namespace

HeadPose HeadPoseModel::evaluate(ID3D11Buffer* preprocessed) {
    HeadPose pose{};
    if (!preprocessed) return pose;

    auto* ctx = d3d_.context();
    const UINT elements = 3u * inputSize_ * inputSize_;

    // 1. Copy the GPU tensor into the CPU-readable staging buffer and map it.
    //    (Runs on the inference worker thread, same thread as the preprocessor
    //    Dispatch, so the immediate context is used single-threaded.)
    ctx->CopyResource(staging_.get(), preprocessed);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        log::warn(L"HeadPoseModel: staging Map failed.");
        return pose;
    }
    // The shader writes RWStructuredBuffer<min16float> with a 4-byte stride.
    // Compiled by fxc without native 16-bit types, min16float is stored as a
    // full 32-bit float, so each slot is a plain float32 — read it directly
    // (this also matches the exported ONNX graph's tensor(float) input).
    std::memcpy(inputData_.data(), mapped.pData, elements * sizeof(float));
    ctx->Unmap(staging_.get(), 0);

    // 2. Bind the float32 [1,3,H,W] input and run synchronous inference.
    try {
        const std::vector<int64_t> shape{
            1, 3, static_cast<int64_t>(inputSize_),
            static_cast<int64_t>(inputSize_)};
        auto input = TensorFloat::CreateFromArray(shape, inputData_);

        binding_.Clear();
        binding_.Bind(inputName_, input);

        auto results = session_.Evaluate(binding_, L"");
        auto out = results.Outputs().Lookup(outputName_).try_as<TensorFloat>();
        if (!out) {
            log::warn(L"HeadPoseModel: output is not a float tensor.");
            return pose;
        }

        auto view = out.GetAsVectorView();
        if (view.Size() < 9) {
            log::warn(L"HeadPoseModel: unexpected output size {}.", view.Size());
            return pose;
        }

        float R[9];
        for (uint32_t i = 0; i < 9; ++i) R[i] = view.GetAt(i);
        decodeEuler(R, pose.yaw, pose.pitch, pose.roll);

        // No face detector yet, so we can't score "is a face present". Assume
        // the user is at the camera and report full confidence; replace this
        // with the detector's score once it lands (see HANDOFF.md). Reject
        // implausible poses (NaN / wildly out of range) as low confidence.
        const bool plausible = std::isfinite(pose.yaw) &&
                               std::isfinite(pose.pitch) &&
                               std::abs(pose.yaw) <= 90.0f &&
                               std::abs(pose.pitch) <= 90.0f;
        pose.confidence = plausible ? 1.0f : 0.0f;
    } catch (winrt::hresult_error const& e) {
        log::warn(L"HeadPoseModel: Evaluate failed: {}",
                  std::wstring{e.message()});
        pose.confidence = 0.0f;
    }
    return pose;
}

}  // namespace monitour::inference

#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <winrt/base.h>

#include <onnxruntime_cxx_api.h>

#include "DeviceFactory.h"
#include "HeadPose.h"

namespace monitour::d3d { class D3DContext; class Nv12ToTensor; }
namespace monitour::capture { struct Frame; }

namespace monitour::inference {

// Owns an ORT Session built against the new Windows ML EP catalog. Reads the
// preprocessed CHW fp32 tensor from a D3D11 staging buffer, evaluates the
// model, returns a HeadPose.
class HeadPoseModel {
public:
    HeadPoseModel(d3d::D3DContext& d3d,
                  const std::filesystem::path& modelPath,
                  DeviceChoice choice,
                  const std::filesystem::path& cacheDir,
                  UINT inputSize);
    ~HeadPoseModel();

    HeadPoseModel(const HeadPoseModel&) = delete;
    HeadPoseModel& operator=(const HeadPoseModel&) = delete;

    HeadPose evaluate(ID3D11Buffer* preprocessed);

    [[nodiscard]] UINT inputSize() const noexcept { return inputSize_; }
    [[nodiscard]] const ResolvedDevice& device() const noexcept { return resolved_; }

private:
    d3d::D3DContext& d3d_;
    UINT             inputSize_;
    ResolvedDevice   resolved_;

    std::unique_ptr<Ort::Session> session_;
    std::string  inputName_;
    std::string  outputName_;

    winrt::com_ptr<ID3D11Buffer> staging_;
    std::vector<float>           inputData_;

    // Diagnostics — flushed every 50 evaluations.
    uint64_t evalCount_{0};
    uint64_t plausibleCount_{0};
    float yawMin_{std::numeric_limits<float>::infinity()};
    float yawMax_{-std::numeric_limits<float>::infinity()};
    float pitchMin_{std::numeric_limits<float>::infinity()};
    float pitchMax_{-std::numeric_limits<float>::infinity()};
};

}  // namespace monitour::inference

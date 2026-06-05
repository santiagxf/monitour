#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <filesystem>
#include <vector>

#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/base.h>

#include "DeviceFactory.h"
#include "HeadPose.h"

namespace monitour::d3d { class D3DContext; class Nv12ToTensor; }
namespace monitour::capture { struct Frame; }

namespace monitour::inference {

// Owns the LearningModelSession + reusable binding. Evaluates a single
// preprocessed NV12 frame and returns a HeadPose.
class HeadPoseModel {
public:
    HeadPoseModel(d3d::D3DContext& d3d,
                  const std::filesystem::path& modelPath,
                  DeviceSelection device,
                  UINT inputSize);
    ~HeadPoseModel();

    HeadPoseModel(const HeadPoseModel&) = delete;
    HeadPoseModel& operator=(const HeadPoseModel&) = delete;

    // Run inference. `preprocessed` is the output buffer of Nv12ToTensor
    // (planar CHW float16, 3*inputSize*inputSize elements).
    HeadPose evaluate(ID3D11Buffer* preprocessed);

    [[nodiscard]] UINT inputSize() const noexcept { return inputSize_; }
    [[nodiscard]] const DeviceSelection& device() const noexcept { return device_; }

private:
    d3d::D3DContext& d3d_;
    DeviceSelection  device_;
    UINT             inputSize_;

    winrt::Windows::AI::MachineLearning::LearningModel        model_{nullptr};
    winrt::Windows::AI::MachineLearning::LearningModelSession session_{nullptr};
    winrt::Windows::AI::MachineLearning::LearningModelBinding binding_{nullptr};
    std::wstring inputName_;
    std::wstring outputName_;

    // CPU-readback staging for the preprocessed tensor + reusable float32 input
    // (the exported ONNX graph takes tensor(float); the preprocessor emits f16).
    winrt::com_ptr<ID3D11Buffer> staging_;
    std::vector<float>           inputData_;
};

}  // namespace monitour::inference

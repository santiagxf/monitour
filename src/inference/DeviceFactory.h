#pragma once
#include <filesystem>
#include <string>

#include <winrt/Windows.AI.MachineLearning.h>

namespace monitour::inference {

enum class DeviceChoice {
    Auto,        // NPU → DirectX → CPU
    ForceNpu,
    ForceDirectX,
    ForceCpu,
};

struct DeviceSelection {
    winrt::Windows::AI::MachineLearning::LearningModelDevice device{nullptr};
    winrt::Windows::AI::MachineLearning::LearningModelDeviceKind kind{
        winrt::Windows::AI::MachineLearning::LearningModelDeviceKind::Default};
    std::wstring description;
    bool isNpu{false};
};

// Picks the best available LearningModelDevice per DeviceChoice. Logs the
// chosen device once so we can verify in the field.
DeviceSelection selectDevice(DeviceChoice choice);

// Path to the model variant best matched to the selected device:
//   NPU      → 6drepnet.int8.onnx
//   DirectX  → 6drepnet.fp16.onnx
//   CPU      → 6drepnet.fp16.onnx (acceptable for diagnostics only)
std::filesystem::path modelPathFor(const DeviceSelection& sel,
                                   const std::filesystem::path& modelDir);

}  // namespace monitour::inference

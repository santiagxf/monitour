#include "DeviceFactory.h"

#include "util/Logging.h"

namespace monitour::inference {

using namespace winrt::Windows::AI::MachineLearning;

namespace {

DeviceSelection tryKind(LearningModelDeviceKind kind, std::wstring label) {
    DeviceSelection sel;
    try {
        sel.device = LearningModelDevice{kind};
        sel.kind = kind;
        sel.description = std::move(label);
        sel.isNpu = (kind == LearningModelDeviceKind::DirectXHighPerformance);
        // Note: Windows ML's "NPU" routing is exposed via LearningModelDevice
        // policy hints in the newer Microsoft.Windows.AI.MachineLearning
        // namespace. Older Windows.AI.MachineLearning uses Direct3D fallback.
        // The actual NPU selection is verified by the runtime; we log what we
        // requested and what we got.
    } catch (winrt::hresult_error const& e) {
        log::warn(L"LearningModelDevice({}) failed: 0x{:x} {}",
                  sel.description, static_cast<unsigned>(e.code().value), e.message());
    }
    return sel;
}

}  // namespace

DeviceSelection selectDevice(DeviceChoice choice) {
    DeviceSelection sel;
    switch (choice) {
        case DeviceChoice::ForceCpu:
            sel = tryKind(LearningModelDeviceKind::Cpu, L"CPU");
            break;
        case DeviceChoice::ForceDirectX:
            sel = tryKind(LearningModelDeviceKind::DirectX, L"DirectX");
            break;
        case DeviceChoice::ForceNpu:
            // Best-effort: request high-performance D3D; NPU routing is
            // selected by the runtime if a compatible NPU is present.
            sel = tryKind(LearningModelDeviceKind::DirectXHighPerformance,
                          L"DirectXHighPerformance (NPU-preferred)");
            break;
        case DeviceChoice::Auto:
        default: {
            // Try NPU-preferring path first; fall back to DirectX, then CPU.
            sel = tryKind(LearningModelDeviceKind::DirectXHighPerformance,
                          L"DirectXHighPerformance (NPU-preferred)");
            if (!sel.device) {
                sel = tryKind(LearningModelDeviceKind::DirectX, L"DirectX");
            }
            if (!sel.device) {
                sel = tryKind(LearningModelDeviceKind::Cpu, L"CPU");
            }
            break;
        }
    }

    if (sel.device) {
        log::info(L"Inference device selected: {}", sel.description);
    } else {
        log::error(L"No LearningModelDevice could be created.");
    }
    return sel;
}

std::filesystem::path modelPathFor(const DeviceSelection& sel,
                                   const std::filesystem::path& modelDir) {
    if (sel.isNpu) {
        auto p = modelDir / L"6drepnet.int8.onnx";
        if (std::filesystem::exists(p)) return p;
        log::warn(L"INT8 model missing; falling back to FP16");
    }
    return modelDir / L"6drepnet.fp16.onnx";
}

}  // namespace monitour::inference

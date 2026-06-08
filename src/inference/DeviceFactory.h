#pragma once
#include <filesystem>
#include <memory>
#include <string>

#include <onnxruntime_cxx_api.h>

namespace monitour::inference {

enum class DeviceChoice {
    Auto,        // prefer NPU; fall back to GPU then CPU
    ForceNpu,    // diagnostic: throw if no NPU EP is registered
    ForceGpu,    // diagnostic: pin to GPU
    ForceCpu,    // diagnostic: pin to CPU
};

// What the runtime actually resolved to *before* the session is built. We
// trust the explicitly-selected EpDevice, not a wished-for policy.
struct ResolvedDevice {
    std::wstring epName;       // e.g. "OpenVINOExecutionProvider"
    std::wstring deviceType;   // "NPU" | "GPU" | "CPU"
    std::wstring vendor;       // "Intel" | "Microsoft" | "AMD" | ...
    bool onNpu{false};
};

// Shared process-wide ORT environment. Created on first call, destroyed at
// process exit. The Windows ML EP catalog is bound to this env.
Ort::Env& ortEnv();

// Pick the best EpDevice for `choice` and build a SessionOptions that
// targets it. Sets the OpenVINO cache_dir provider option so first-run NPU
// compile is amortized across launches. Logs the chosen device. Throws
// std::runtime_error if no acceptable device is found.
struct SessionPlan {
    std::unique_ptr<Ort::SessionOptions> options;
    ResolvedDevice resolved;
};
SessionPlan makeSessionPlan(DeviceChoice choice,
                            const std::filesystem::path& cacheDir);

// Path to the model variant for the target device. Today this is always the
// FP16 model — the NPU runs FP16 natively and INT8 needs per-machine
// activation calibration that isn't worth the deployment hazard.
std::filesystem::path modelPathFor(const std::filesystem::path& modelDir);

}  // namespace monitour::inference

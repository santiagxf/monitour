#include "DeviceFactory.h"

#include <Windows.h>
#include <WinMLEpCatalog.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/Logging.h"

namespace monitour::inference {

namespace {

std::once_flag g_envOnce;
std::unique_ptr<Ort::Env> g_env;

std::wstring toWide(const char* s) {
    if (!s) return {};
    std::string_view v{s};
    return std::wstring(v.begin(), v.end());
}

// Map DeviceChoice → ordered list of acceptable OrtHardwareDeviceType values
// to try, from most-preferred to fallback. Empty list = "anything".
std::vector<OrtHardwareDeviceType> acceptableDevices(DeviceChoice c) {
    switch (c) {
        case DeviceChoice::ForceNpu:
            return {OrtHardwareDeviceType_NPU};
        case DeviceChoice::ForceGpu:
            return {OrtHardwareDeviceType_GPU};
        case DeviceChoice::ForceCpu:
            return {OrtHardwareDeviceType_CPU};
        case DeviceChoice::Auto:
        default:
            return {OrtHardwareDeviceType_NPU,
                    OrtHardwareDeviceType_GPU,
                    OrtHardwareDeviceType_CPU};
    }
}

const char* devTypeName(OrtHardwareDeviceType t) {
    switch (t) {
        case OrtHardwareDeviceType_CPU: return "CPU";
        case OrtHardwareDeviceType_GPU: return "GPU";
        case OrtHardwareDeviceType_NPU: return "NPU";
        default:                        return "?";
    }
}

}  // namespace

Ort::Env& ortEnv() {
    std::call_once(g_envOnce, []{
        g_env = std::make_unique<Ort::Env>(
            ORT_LOGGING_LEVEL_WARNING, "monitour");
    });
    return *g_env;
}

namespace {

std::string readStringFromEp(WinMLEpHandle ep,
                             HRESULT (*sizeFn)(WinMLEpHandle, size_t*),
                             HRESULT (*readFn)(WinMLEpHandle, size_t,
                                               char*, size_t*)) {
    size_t needed = 0;
    if (FAILED(sizeFn(ep, &needed)) || needed == 0) return {};
    std::string buf(needed, '\0');
    size_t used = 0;
    if (FAILED(readFn(ep, buf.size(), buf.data(), &used))) return {};
    // Trim trailing NULs (the API counts the terminator into `used`).
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
}

// Walk the Windows ML EP catalog, install any EP that isn't ready yet via
// Windows Update, then hand each EP's loader DLL to ORT so it shows up in
// GetEpDevices. Idempotent — safe to call once at startup.
void registerWindowsMlEps(Ort::Env& env) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;

    WinMLEpCatalogHandle catalog = nullptr;
    if (FAILED(WinMLEpCatalogCreate(&catalog)) || !catalog) {
        log::warn(L"WinMLEpCatalogCreate failed; only built-in EPs available.");
        return;
    }

    struct Acc { std::vector<WinMLEpHandle> eps; };
    Acc acc;
    WinMLEpCatalogEnumProviders(catalog,
        [](WinMLEpHandle ep, const WinMLEpInfo* /*info*/, void* ctx) -> BOOL {
            static_cast<Acc*>(ctx)->eps.push_back(ep);
            return TRUE;
        }, &acc);

    log::info(L"Windows ML EP catalog: {} provider(s) discovered.",
              acc.eps.size());

    for (WinMLEpHandle ep : acc.eps) {
        const auto name    = readStringFromEp(ep, WinMLEpGetNameSize,
                                              WinMLEpGetName);
        const auto libPath = readStringFromEp(ep, WinMLEpGetLibraryPathSize,
                                              WinMLEpGetLibraryPath);
        WinMLEpReadyState state{};
        WinMLEpGetReadyState(ep, &state);

        if (state != WinMLEpReadyState_Ready) {
            // EP package is registered with the OS but the binary isn't on
            // disk yet (or is being updated). Pull it down synchronously —
            // this can take a few seconds on first launch.
            log::info(L"  - {}: not ready, installing via Windows Update...",
                      toWide(name.c_str()));
            HRESULT hr = WinMLEpEnsureReady(ep);
            if (FAILED(hr)) {
                log::warn(L"    WinMLEpEnsureReady failed: 0x{:x}",
                          static_cast<unsigned>(hr));
                continue;
            }
            // libraryPath only resolves after the EP is ready.
            const auto resolvedPath = readStringFromEp(
                ep, WinMLEpGetLibraryPathSize, WinMLEpGetLibraryPath);
            if (resolvedPath.empty()) {
                log::warn(L"    No library path after EnsureReady.");
                continue;
            }
            try {
                env.RegisterExecutionProviderLibrary(
                    name.c_str(),
                    std::wstring(resolvedPath.begin(), resolvedPath.end()));
                log::info(L"    registered '{}' from {}",
                          toWide(name.c_str()),
                          toWide(resolvedPath.c_str()));
            } catch (Ort::Exception const& e) {
                log::warn(L"    ORT register failed: {}",
                          toWide(e.what()));
            }
            continue;
        }

        // EP is on disk. Hand its loader to ORT.
        if (libPath.empty()) {
            log::warn(L"  - {}: ready but no library path; skipping.",
                      toWide(name.c_str()));
            continue;
        }
        try {
            env.RegisterExecutionProviderLibrary(
                name.c_str(),
                std::wstring(libPath.begin(), libPath.end()));
            log::info(L"  - registered '{}' from {}",
                      toWide(name.c_str()), toWide(libPath.c_str()));
        } catch (Ort::Exception const& e) {
            log::warn(L"    ORT register failed: {}", toWide(e.what()));
        }
    }

    WinMLEpCatalogRelease(catalog);
}

}  // namespace

SessionPlan makeSessionPlan(DeviceChoice choice,
                            const std::filesystem::path& cacheDir) {
    auto& env = ortEnv();

    // Install + register every Windows-ML-delivered EP with ORT. Without
    // this step env.GetEpDevices() only returns the built-in CPU + DML; the
    // OpenVINO NPU EP never shows up.
    registerWindowsMlEps(env);

    // Enumerate every (EP, hardware device) pair Windows ML knows about.
    // The list is dynamic — driver/EP updates can add or remove entries
    // between launches — so log it on every startup for diagnosability.
    const std::vector<Ort::ConstEpDevice> devices = env.GetEpDevices();
    log::info(L"Resolved ORT execution providers: {}", devices.size());
    for (const auto& d : devices) {
        log::info(L"  - {} on {} ({})",
                  toWide(d.EpName()),
                  toWide(devTypeName(d.Device().Type())),
                  toWide(d.Device().Vendor()));
    }

    Ort::ConstEpDevice picked{nullptr};
    for (auto wanted : acceptableDevices(choice)) {
        for (const auto& d : devices) {
            if (d.Device().Type() == wanted) {
                picked = d;
                break;
            }
        }
        if (picked) break;
    }
    if (!picked) {
        throw std::runtime_error{
            "No execution-provider device matched the requested DeviceChoice."};
    }

    SessionPlan plan;
    plan.resolved.epName     = toWide(picked.EpName());
    plan.resolved.deviceType = toWide(devTypeName(picked.Device().Type()));
    plan.resolved.vendor     = toWide(picked.Device().Vendor());
    plan.resolved.onNpu      =
        (picked.Device().Type() == OrtHardwareDeviceType_NPU);

    plan.options = std::make_unique<Ort::SessionOptions>();
    // Single-threaded: the EP owns its scheduler. ORT's CPU thread pool
    // would only fight the desktop for cycles.
    plan.options->SetIntraOpNumThreads(1);
    plan.options->SetInterOpNumThreads(1);
    plan.options->SetExecutionMode(ORT_SEQUENTIAL);
    // Let the EP compiler see the unmodified graph; OpenVINO does its own
    // NPU-aware optimization that ORT's high-level passes interfere with.
    plan.options->SetGraphOptimizationLevel(ORT_DISABLE_ALL);

    // OpenVINO NPU compile cache. First run pays a multi-second compile;
    // the cache makes subsequent launches near-instant.
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);

    std::unordered_map<std::string, std::string> epOptions;
    if (std::string{picked.EpName()} == "OpenVINOExecutionProvider") {
        epOptions["cache_dir"] = cacheDir.string();
    }
    plan.options->AppendExecutionProvider_V2(env, {picked}, epOptions);

    log::info(L"Inference device picked: {} on {} ({})",
              plan.resolved.epName, plan.resolved.deviceType,
              plan.resolved.vendor);
    return plan;
}

std::filesystem::path modelPathFor(const std::filesystem::path& modelDir) {
    return modelDir / L"6drepnet.fp16.onnx";
}

}  // namespace monitour::inference

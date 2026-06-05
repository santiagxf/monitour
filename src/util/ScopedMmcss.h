#pragma once
#include <Windows.h>
#include <string_view>

namespace monitour::util {

// RAII wrapper around AvSetMmThreadCharacteristicsW / AvRevertMmThreadCharacteristics.
// Opting the current thread into MMCSS prevents Windows from demoting it via
// EcoQoS / Modern Standby, which is load-bearing for predictable p99 latency
// on the capture and inference threads.
class ScopedMmcss {
public:
    // task: an MMCSS task profile name. Built-in profiles include
    // L"Capture", L"Pro Audio", L"Audio", L"Playback", L"Window Manager".
    // L"Capture" matches the camera capture loop; L"Pro Audio" gives the
    // tightest scheduling guarantees and suits the inference loop.
    explicit ScopedMmcss(std::wstring_view task) noexcept;
    ~ScopedMmcss();

    ScopedMmcss(const ScopedMmcss&) = delete;
    ScopedMmcss& operator=(const ScopedMmcss&) = delete;

    [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_{nullptr};
    DWORD  taskIndex_{0};
};

}  // namespace monitour::util

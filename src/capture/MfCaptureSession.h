#pragma once
#include <Windows.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <winrt/base.h>

#include <atomic>
#include <thread>

#include "FrameSlot.h"

namespace monitour::d3d { class D3DContext; }

namespace monitour::capture {

struct CaptureConfig {
    UINT preferredWidth  = 640;
    UINT preferredHeight = 480;
    UINT preferredFps    = 60;     // falls back to 30 if camera caps lower
    bool requireNv12     = true;
};

// Opens the default video capture device, configures it for NV12 output on
// the shared D3D11 device (zero-copy via IMFDXGIDeviceManager), and runs a
// dedicated capture thread that publishes each decoded frame to `slot`.
class MfCaptureSession : public IMFSourceReaderCallback {
public:
    MfCaptureSession(d3d::D3DContext& d3d, FrameSlot& slot, CaptureConfig cfg);
    ~MfCaptureSession();

    MfCaptureSession(const MfCaptureSession&) = delete;
    MfCaptureSession& operator=(const MfCaptureSession&) = delete;

    void start();
    void stop();

    // Throttle: when face-lost, the orchestrator sets this to 1000 ms and
    // the reader pauses between samples accordingly.
    void setInterFrameDelayMs(DWORD ms) { delayMs_.store(ms, std::memory_order_relaxed); }

    // --- IMFSourceReaderCallback ---
    HRESULT STDMETHODCALLTYPE OnReadSample(
        HRESULT hrStatus, DWORD streamIndex, DWORD streamFlags,
        LONGLONG timestamp, IMFSample* sample) noexcept override;
    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent*) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) noexcept override { return S_OK; }

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override;
    ULONG   STDMETHODCALLTYPE AddRef()  noexcept override;
    ULONG   STDMETHODCALLTYPE Release() noexcept override;

private:
    void openSourceReader();
    HRESULT requestNextSample() noexcept;

    d3d::D3DContext&  d3d_;
    FrameSlot&        slot_;
    CaptureConfig     cfg_;

    winrt::com_ptr<IMFMediaSource>   source_;
    winrt::com_ptr<IMFSourceReader>  reader_;

    std::atomic<ULONG>   refs_{1};
    std::atomic<bool>    running_{false};
    std::atomic<DWORD>   delayMs_{0};
    std::atomic<uint64_t> seq_{0};
};

}  // namespace monitour::capture

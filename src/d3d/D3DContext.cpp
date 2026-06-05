#include "D3DContext.h"

#include <mfapi.h>

#include "util/ComUtil.h"
#include "util/Logging.h"

namespace monitour::d3d {

D3DContext::D3DContext() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL chosen{};
    MONITOUR_CHECK_HR(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        device_.put(), &chosen, context_.put()));

    // Multi-threaded protection: Media Foundation will touch the device from
    // its own threads; we touch it from the inference thread.
    winrt::com_ptr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(mt.put())))) {
        mt->SetMultithreadProtected(TRUE);
    }

    MONITOUR_CHECK_HR(MFCreateDXGIDeviceManager(&mfResetToken_, mfManager_.put()));
    MONITOUR_CHECK_HR(mfManager_->ResetDevice(device_.get(), mfResetToken_));

    log::info(L"D3DContext: created D3D11 device, feature level 0x{:x}",
              static_cast<unsigned>(chosen));
}

D3DContext::~D3DContext() = default;

}  // namespace monitour::d3d

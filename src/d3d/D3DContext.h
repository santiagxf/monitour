#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mfobjects.h>
#include <winrt/base.h>

namespace monitour::d3d {

// One D3D11 device, shared by:
//   - Media Foundation (frames decoded directly into ID3D11Texture2D),
//   - the NV12-to-tensor compute shader,
//   - Windows ML / DirectML (via cross-API texture sharing).
//
// Created once at startup; lives for the program's lifetime.
class D3DContext {
public:
    D3DContext();
    ~D3DContext();

    D3DContext(const D3DContext&) = delete;
    D3DContext& operator=(const D3DContext&) = delete;

    [[nodiscard]] ID3D11Device*        device()        const noexcept { return device_.get(); }
    [[nodiscard]] ID3D11DeviceContext* context()       const noexcept { return context_.get(); }
    [[nodiscard]] IMFDXGIDeviceManager* mfManager()    const noexcept { return mfManager_.get(); }
    [[nodiscard]] UINT                  mfResetToken() const noexcept { return mfResetToken_; }

private:
    winrt::com_ptr<ID3D11Device>         device_;
    winrt::com_ptr<ID3D11DeviceContext>  context_;
    winrt::com_ptr<IMFDXGIDeviceManager> mfManager_;
    UINT                                 mfResetToken_{0};
};

}  // namespace monitour::d3d

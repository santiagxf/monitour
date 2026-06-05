#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <filesystem>
#include <string>

namespace monitour::d3d {

class D3DContext;

// Loads the precompiled Nv12ToTensor.cso compute shader and dispatches it
// against a (Y, UV) NV12 source texture pair, producing a planar CHW
// float16 tensor in a ID3D11Buffer suitable for binding to Windows ML.
class Nv12ToTensor {
public:
    // dstSize: side of the square model input (e.g., 224 for 6DRepNet).
    Nv12ToTensor(D3DContext& d3d, std::filesystem::path csoPath, UINT dstSize);
    ~Nv12ToTensor();

    Nv12ToTensor(const Nv12ToTensor&) = delete;
    Nv12ToTensor& operator=(const Nv12ToTensor&) = delete;

    // src must be an NV12 ID3D11Texture2D (typically delivered by Media
    // Foundation via IMFDXGIBuffer). subresource is the array slice index.
    // faceBox is in source-frame pixel coordinates; the shader will center-
    // crop to a square containing it, then resize to dstSize×dstSize.
    void Dispatch(ID3D11Texture2D* src,
                  UINT subresource,
                  RECT faceBox);

    // The output buffer holding the float16 CHW tensor of size 3*dstSize*dstSize.
    [[nodiscard]] ID3D11Buffer* outputBuffer() const noexcept { return outputBuffer_.get(); }
    [[nodiscard]] UINT          dstSize()      const noexcept { return dstSize_; }

private:
    D3DContext& d3d_;
    UINT        dstSize_;
    winrt::com_ptr<ID3D11ComputeShader>     shader_;
    winrt::com_ptr<ID3D11Buffer>            outputBuffer_;
    winrt::com_ptr<ID3D11UnorderedAccessView> outputUav_;
    winrt::com_ptr<ID3D11Buffer>            cbuffer_;
    winrt::com_ptr<ID3D11SamplerState>      sampler_;
};

}  // namespace monitour::d3d

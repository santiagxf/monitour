#include "FaceDetector.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include <vector>

#include "d3d/D3DContext.h"
#include "util/ComUtil.h"
#include "util/Logging.h"

namespace monitour::inference {

using namespace winrt::Windows::Graphics::Imaging;
using winrt::Windows::Security::Cryptography::CryptographicBuffer;

bool FaceDetector::create() {
    if (!winrt::Windows::Media::FaceAnalysis::FaceDetector::IsSupported()) {
        log::warn(L"FaceDetector unsupported on this platform; using center crop.");
        return false;
    }
    try {
        detector_ = winrt::Windows::Media::FaceAnalysis::FaceDetector::CreateAsync().get();
    } catch (winrt::hresult_error const& e) {
        log::warn(L"FaceDetector::CreateAsync failed: {}", std::wstring{e.message()});
        detector_ = nullptr;
        return false;
    }
    log::info(L"FaceDetector ready (Gray8 luma).");
    return true;
}

std::optional<RECT> FaceDetector::detect(d3d::D3DContext& d3d,
                                         ID3D11Texture2D* nv12,
                                         UINT subresource) {
    if (!detector_ || !nv12) return std::nullopt;

    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    const UINT w = desc.Width;
    const UINT h = desc.Height;
    if (w == 0 || h == 0) return std::nullopt;

    auto* dev = d3d.device();
    auto* ctx = d3d.context();

    // (Re)create the single-slice NV12 staging texture if the size changed.
    if (!staging_ || stagingW_ != w || stagingH_ != h) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width            = w;
        sd.Height           = h;
        sd.MipLevels        = 1;
        sd.ArraySize        = 1;
        sd.Format           = desc.Format;  // NV12
        sd.SampleDesc.Count = 1;
        sd.Usage            = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
        staging_ = nullptr;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, staging_.put()))) {
            log::warn(L"FaceDetector: staging texture create failed.");
            return std::nullopt;
        }
        stagingW_ = w;
        stagingH_ = h;
    }

    // Copy just the requested array slice into the single-slice staging.
    ctx->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0, nv12, subresource,
                               nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return std::nullopt;
    }
    // Compact the Y plane (first `h` rows) into a contiguous Gray8 buffer.
    std::vector<uint8_t> gray(static_cast<size_t>(w) * h);
    const auto* base = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < h; ++y) {
        std::memcpy(gray.data() + static_cast<size_t>(y) * w,
                    base + static_cast<size_t>(y) * mapped.RowPitch, w);
    }
    ctx->Unmap(staging_.get(), 0);

    try {
        auto buffer = CryptographicBuffer::CreateFromByteArray(gray);
        auto bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            buffer, BitmapPixelFormat::Gray8, static_cast<int32_t>(w),
            static_cast<int32_t>(h));

        auto faces = detector_.DetectFacesAsync(bitmap).get();
        if (faces.Size() == 0) return std::nullopt;

        // Keep the largest face (closest / most prominent user).
        BitmapBounds best{};
        uint32_t bestArea = 0;
        for (auto const& face : faces) {
            const auto b = face.FaceBox();
            const uint32_t area = b.Width * b.Height;
            if (area > bestArea) {
                bestArea = area;
                best = b;
            }
        }
        if (bestArea == 0) return std::nullopt;

        RECT r{};
        r.left   = static_cast<LONG>(best.X);
        r.top    = static_cast<LONG>(best.Y);
        r.right  = static_cast<LONG>(best.X + best.Width);
        r.bottom = static_cast<LONG>(best.Y + best.Height);
        return r;
    } catch (winrt::hresult_error const& e) {
        log::warn(L"FaceDetector: DetectFacesAsync failed: {}",
                  std::wstring{e.message()});
        return std::nullopt;
    }
}

}  // namespace monitour::inference

#include "FaceDetector.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
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

    // Verify the platform actually supports Gray8 — that's what we feed it
    // (the Y plane of the NV12 capture frame). If not, the caller is better
    // off falling back to the centered crop.
    bool gray8Supported = false;
    try {
        gray8Supported =
            winrt::Windows::Media::FaceAnalysis::FaceDetector::
                IsBitmapPixelFormatSupported(BitmapPixelFormat::Gray8);
    } catch (winrt::hresult_error const& e) {
        log::warn(L"FaceDetector::IsBitmapPixelFormatSupported(Gray8) threw: {}",
                  std::wstring{e.message()});
    }
    if (!gray8Supported) {
        log::warn(L"FaceDetector does not support Gray8 on this platform; "
                  L"disabling detector and using center crop.");
        detector_ = nullptr;
        return false;
    }

    log::info(L"FaceDetector ready (Gray8 luma).");
    return true;
}

std::optional<RECT> FaceDetector::detect(d3d::D3DContext& d3d,
                                         ID3D11Texture2D* nv12,
                                         UINT subresource) {
    ++attempts_;
    auto bump = [this](Outcome which) {
        ++outcomes_[static_cast<size_t>(which)];
        maybeFlushDiagnostics();
    };

    if (!detector_) { bump(Outcome::NoDetector); return std::nullopt; }
    if (!nv12)      { bump(Outcome::NullTexture); return std::nullopt; }

    D3D11_TEXTURE2D_DESC desc{};
    nv12->GetDesc(&desc);
    const UINT w = desc.Width;
    const UINT h = desc.Height;
    if (w == 0 || h == 0) { bump(Outcome::ZeroSizeFrame); return std::nullopt; }

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
            bump(Outcome::StagingCreateFailed);
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
        bump(Outcome::MapFailed);
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

    // Cheap luma stats: tells us at-a-glance if the frame is black (sensor
    // not ready / wrong camera), saturated (overexposed), or flat (IR-ish).
    // Sample every 64 pixels — ~4800 samples on 640×480 is enough.
    {
        uint64_t sum = 0;
        uint8_t  mn  = 255, mx = 0;
        size_t   n   = 0;
        const size_t total = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < total; i += 64) {
            const uint8_t v = gray[i];
            sum += v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            ++n;
        }
        lumaMeanSum_ += n ? sum / n : 0;
        ++lumaSamples_;
        lumaMin_ = std::min(lumaMin_, mn);
        lumaMax_ = std::max(lumaMax_, mx);
    }

    try {
        auto buffer = CryptographicBuffer::CreateFromByteArray(gray);
        auto bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            buffer, BitmapPixelFormat::Gray8, static_cast<int32_t>(w),
            static_cast<int32_t>(h));

        auto faces = detector_.DetectFacesAsync(bitmap).get();
        faceCandidates_ += faces.Size();
        if (faces.Size() == 0) {
            bump(Outcome::NoFaceFound);
            return std::nullopt;
        }

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
        if (bestArea == 0) {
            bump(Outcome::ZeroAreaFace);
            return std::nullopt;
        }

        RECT r{};
        r.left   = static_cast<LONG>(best.X);
        r.top    = static_cast<LONG>(best.Y);
        r.right  = static_cast<LONG>(best.X + best.Width);
        r.bottom = static_cast<LONG>(best.Y + best.Height);
        bump(Outcome::Success);
        return r;
    } catch (winrt::hresult_error const& e) {
        log::warn(L"FaceDetector: DetectFacesAsync failed: 0x{:x} {}",
                  static_cast<unsigned>(e.code().value),
                  std::wstring{e.message()});
        bump(Outcome::DetectThrew);
        return std::nullopt;
    }
}

void FaceDetector::noteUnstableBox() noexcept {
    ++outcomes_[static_cast<size_t>(Outcome::UnstableBox)];
}

void FaceDetector::maybeFlushDiagnostics() {
    constexpr uint64_t kFlushEvery = 50;  // ~12s at 4Hz idle cadence
    if (attempts_ % kFlushEvery != 0) return;

    const uint64_t success  = outcomes_[static_cast<size_t>(Outcome::Success)];
    const uint64_t noFace   = outcomes_[static_cast<size_t>(Outcome::NoFaceFound)];
    const uint64_t threw    = outcomes_[static_cast<size_t>(Outcome::DetectThrew)];
    const uint64_t mapFail  = outcomes_[static_cast<size_t>(Outcome::MapFailed)];
    const uint64_t unstable = outcomes_[static_cast<size_t>(Outcome::UnstableBox)];
    const uint32_t lumaMean =
        lumaSamples_ ? static_cast<uint32_t>(lumaMeanSum_ / lumaSamples_) : 0;

    log::info(L"FaceDetect: {} attempts — success={} noFace={} threw={} "
              L"mapFail={} unstable={} | luma min/mean/max={}/{}/{} | "
              L"candidates/attempt={:.2f}",
              attempts_, success, noFace, threw, mapFail, unstable,
              static_cast<uint32_t>(lumaMin_), lumaMean,
              static_cast<uint32_t>(lumaMax_),
              attempts_ ? static_cast<double>(faceCandidates_) / attempts_ : 0.0);

    // Reset luma window so the next flush reflects the *current* lighting,
    // not an all-time-low/high. Keep cumulative attempt/outcome counts.
    lumaMeanSum_  = 0;
    lumaSamples_  = 0;
    lumaMin_      = 255;
    lumaMax_      = 0;
}

}  // namespace monitour::inference

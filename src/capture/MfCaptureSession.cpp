#include "MfCaptureSession.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include "d3d/D3DContext.h"
#include "util/ComUtil.h"
#include "util/Logging.h"

namespace monitour::capture {

namespace {

// Pick the closest media type the camera advertises: same subtype, largest
// resolution ≤ preferred, highest framerate. If preferredFps unattainable,
// take the next best.
winrt::com_ptr<IMFMediaType> chooseMediaType(IMFSourceReader* reader,
                                             const CaptureConfig& cfg) {
    winrt::com_ptr<IMFMediaType> best;
    UINT bestScore = 0;

    for (DWORD i = 0;; ++i) {
        winrt::com_ptr<IMFMediaType> mt;
        HRESULT hr = reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, mt.put());
        if (hr == MF_E_NO_MORE_TYPES) break;
        MONITOUR_CHECK_HR(hr);

        GUID subtype{};
        if (FAILED(mt->GetGUID(MF_MT_SUBTYPE, &subtype))) continue;
        if (cfg.requireNv12 && subtype != MFVideoFormat_NV12) continue;

        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, &w, &h))) continue;

        UINT32 numer = 0, denom = 0;
        if (FAILED(MFGetAttributeRatio(mt.get(), MF_MT_FRAME_RATE, &numer, &denom)) ||
            denom == 0) continue;
        UINT fps = numer / denom;

        // Score: prefer fps close to preferred, then resolution close to preferred.
        UINT fpsScore = fps >= cfg.preferredFps ? cfg.preferredFps : fps;
        UINT resScore = (w <= cfg.preferredWidth && h <= cfg.preferredHeight)
                            ? (w + h)
                            : 0;
        UINT score = fpsScore * 10000 + resScore;
        if (score > bestScore) {
            bestScore = score;
            best = mt;
        }
    }

    if (!best) {
        throw std::runtime_error{"No NV12 media type advertised by the camera."};
    }
    return best;
}

}  // namespace

MfCaptureSession::MfCaptureSession(d3d::D3DContext& d3d,
                                   FrameSlot& slot,
                                   CaptureConfig cfg)
    : d3d_{d3d}, slot_{slot}, cfg_{cfg} {
    openSourceReader();
}

MfCaptureSession::~MfCaptureSession() {
    stop();
}

void MfCaptureSession::openSourceReader() {
    // Enumerate first video capture device.
    winrt::com_ptr<IMFAttributes> attr;
    MONITOUR_CHECK_HR(MFCreateAttributes(attr.put(), 1));
    MONITOUR_CHECK_HR(attr->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    MONITOUR_CHECK_HR(MFEnumDeviceSources(attr.get(), &devices, &count));
    if (count == 0) {
        if (devices) CoTaskMemFree(devices);
        throw std::runtime_error{"No video capture devices found."};
    }
    MONITOUR_CHECK_HR(devices[0]->ActivateObject(IID_PPV_ARGS(source_.put())));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);

    winrt::com_ptr<IMFAttributes> readerAttr;
    MONITOUR_CHECK_HR(MFCreateAttributes(readerAttr.put(), 4));
    MONITOUR_CHECK_HR(readerAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                             d3d_.mfManager()));
    MONITOUR_CHECK_HR(readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE));
    MONITOUR_CHECK_HR(readerAttr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE));
    MONITOUR_CHECK_HR(readerAttr->SetUnknown(
        MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this)));

    MONITOUR_CHECK_HR(MFCreateSourceReaderFromMediaSource(
        source_.get(), readerAttr.get(), reader_.put()));

    auto mt = chooseMediaType(reader_.get(), cfg_);

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, &w, &h);
    UINT32 fn = 0, fd = 0;
    MFGetAttributeRatio(mt.get(), MF_MT_FRAME_RATE, &fn, &fd);
    log::info(L"Capture: selected NV12 {}x{} @ {} fps", w, h, fd ? fn / fd : 0);

    MONITOUR_CHECK_HR(reader_->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.get()));
    MONITOUR_CHECK_HR(reader_->SetStreamSelection(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));
}

void MfCaptureSession::start() {
    if (running_.exchange(true)) return;
    log::info(L"Capture: starting");
    if (FAILED(requestNextSample())) {
        running_.store(false);
    }
}

void MfCaptureSession::stop() {
    if (!running_.exchange(false)) return;
    log::info(L"Capture: stopping");
    if (reader_) {
        reader_->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    }
}

HRESULT MfCaptureSession::requestNextSample() noexcept {
    return reader_->ReadSample(
        MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
}

HRESULT STDMETHODCALLTYPE MfCaptureSession::OnReadSample(
    HRESULT hrStatus, DWORD /*streamIndex*/, DWORD streamFlags,
    LONGLONG timestamp, IMFSample* sample) noexcept {
    if (!running_.load(std::memory_order_acquire)) return S_OK;

    if (FAILED(hrStatus)) {
        log::error(L"OnReadSample HRESULT 0x{:x}", static_cast<unsigned>(hrStatus));
    } else if (sample) {
        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(buffer.put()))) {
            winrt::com_ptr<IMFDXGIBuffer> dxgi;
            if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(dxgi.put())))) {
                winrt::com_ptr<ID3D11Texture2D> tex;
                UINT subres = 0;
                if (SUCCEEDED(dxgi->GetResource(IID_PPV_ARGS(tex.put()))) &&
                    SUCCEEDED(dxgi->GetSubresourceIndex(&subres))) {
                    Frame f;
                    f.texture = tex;
                    f.subresource = subres;
                    f.captureQpc = timestamp;
                    f.seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
                    slot_.publish(std::move(f));
                }
            }
        }
    }

    if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        running_.store(false);
        return S_OK;
    }

    DWORD delay = delayMs_.load(std::memory_order_relaxed);
    if (delay) Sleep(delay);

    return requestNextSample();
}

HRESULT STDMETHODCALLTYPE MfCaptureSession::QueryInterface(REFIID riid, void** ppv) noexcept {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MfCaptureSession::AddRef() noexcept {
    return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE MfCaptureSession::Release() noexcept {
    ULONG r = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    // We own the lifetime via stack/unique_ptr; no delete here.
    return r;
}

}  // namespace monitour::capture

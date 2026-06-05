#include "Nv12ToTensor.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "D3DContext.h"
#include "util/ComUtil.h"
#include "util/Logging.h"

namespace monitour::d3d {

namespace {

struct ResizeParams {
    UINT  srcWidth;
    UINT  srcHeight;
    UINT  dstSize;
    UINT  _pad0;
    float cropOriginX;
    float cropOriginY;
    float cropSize;
    float _pad1;
};

std::vector<std::byte> readBinary(const std::filesystem::path& path) {
    std::ifstream f{path, std::ios::binary | std::ios::ate};
    if (!f) {
        throw std::runtime_error{"Failed to open shader: " + path.string()};
    }
    auto size = static_cast<std::streamsize>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

}  // namespace

Nv12ToTensor::Nv12ToTensor(D3DContext& d3d,
                           std::filesystem::path csoPath,
                           UINT dstSize)
    : d3d_{d3d}, dstSize_{dstSize} {
    auto blob = readBinary(csoPath);
    MONITOUR_CHECK_HR(d3d_.device()->CreateComputeShader(
        blob.data(), blob.size(), nullptr, shader_.put()));

    // Output: planar CHW, 3 channels. The shader writes RWStructuredBuffer<min16float>.
    // Structured-buffer rules require StructureByteStride to be a multiple of 4,
    // and min-precision floats are backed by 32-bit storage, so each element is
    // 4 bytes here (not 2).
    D3D11_BUFFER_DESC outDesc{};
    outDesc.ByteWidth = 3 * dstSize_ * dstSize_ * sizeof(uint32_t);
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    // NOTE: cross-API sharing (D3D11_RESOURCE_MISC_SHARED_NTHANDLE) is not set
    // here yet. That flag is only valid when paired with SHARED_KEYEDMUTEX and
    // is needed for the D3D11->D3D12 zero-copy bind in HeadPoseModel::evaluate
    // (still a TODO). Until that path exists, requesting it makes CreateBuffer
    // fail with E_INVALIDARG, so keep this a plain structured buffer.
    outDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    outDesc.StructureByteStride = sizeof(uint32_t);
    MONITOUR_CHECK_HR(d3d_.device()->CreateBuffer(&outDesc, nullptr, outputBuffer_.put()));

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.NumElements = 3 * dstSize_ * dstSize_;
    MONITOUR_CHECK_HR(d3d_.device()->CreateUnorderedAccessView(
        outputBuffer_.get(), &uavDesc, outputUav_.put()));

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(ResizeParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    MONITOUR_CHECK_HR(d3d_.device()->CreateBuffer(&cbDesc, nullptr, cbuffer_.put()));

    D3D11_SAMPLER_DESC sdesc{};
    sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdesc.MaxLOD = D3D11_FLOAT32_MAX;
    MONITOUR_CHECK_HR(d3d_.device()->CreateSamplerState(&sdesc, sampler_.put()));
}

Nv12ToTensor::~Nv12ToTensor() = default;

void Nv12ToTensor::Dispatch(ID3D11Texture2D* src,
                            UINT subresource,
                            RECT faceBox) {
    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    // Two SRVs over the same NV12 texture: plane 0 = Y, plane 1 = UV.
    winrt::com_ptr<ID3D11ShaderResourceView> ySrv, uvSrv;
    D3D11_SHADER_RESOURCE_VIEW_DESC ySrvDesc{};
    ySrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    ySrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    ySrvDesc.Texture2D.MostDetailedMip = 0;
    ySrvDesc.Texture2D.MipLevels = 1;
    MONITOUR_CHECK_HR(d3d_.device()->CreateShaderResourceView(
        src, &ySrvDesc, ySrv.put()));

    D3D11_SHADER_RESOURCE_VIEW_DESC uvSrvDesc = ySrvDesc;
    uvSrvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    MONITOUR_CHECK_HR(d3d_.device()->CreateShaderResourceView(
        src, &uvSrvDesc, uvSrv.put()));

    // Center-square crop containing the detected face box. If faceBox is empty
    // (no detector yet), fall back to the full frame's centered square.
    LONG side = (faceBox.right > faceBox.left && faceBox.bottom > faceBox.top)
                    ? std::max(faceBox.right - faceBox.left,
                               faceBox.bottom - faceBox.top)
                    : std::min<LONG>(srcDesc.Width, srcDesc.Height);
    LONG cx = (faceBox.right > faceBox.left)
                  ? (faceBox.left + faceBox.right) / 2
                  : static_cast<LONG>(srcDesc.Width) / 2;
    LONG cy = (faceBox.bottom > faceBox.top)
                  ? (faceBox.top + faceBox.bottom) / 2
                  : static_cast<LONG>(srcDesc.Height) / 2;

    ResizeParams params{};
    params.srcWidth   = srcDesc.Width;
    params.srcHeight  = srcDesc.Height;
    params.dstSize    = dstSize_;
    params.cropSize   = static_cast<float>(side);
    params.cropOriginX = static_cast<float>(cx) - params.cropSize / 2.f;
    params.cropOriginY = static_cast<float>(cy) - params.cropSize / 2.f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    MONITOUR_CHECK_HR(d3d_.context()->Map(cbuffer_.get(), 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    std::memcpy(mapped.pData, &params, sizeof(params));
    d3d_.context()->Unmap(cbuffer_.get(), 0);

    auto* ctx = d3d_.context();
    ID3D11ShaderResourceView* srvs[] = {ySrv.get(), uvSrv.get()};
    ID3D11UnorderedAccessView* uavs[] = {outputUav_.get()};
    ID3D11Buffer* cbs[] = {cbuffer_.get()};
    ID3D11SamplerState* smps[] = {sampler_.get()};

    ctx->CSSetShader(shader_.get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetSamplers(0, 1, smps);

    UINT groups = (dstSize_ + 15) / 16;
    ctx->Dispatch(groups, groups, 1);

    ID3D11ShaderResourceView* nullSrvs[2] = {};
    ID3D11UnorderedAccessView* nullUavs[1] = {};
    ctx->CSSetShaderResources(0, 2, nullSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

    (void)subresource;  // NV12 source-view of subresource > 0 not yet supported.
}

}  // namespace monitour::d3d

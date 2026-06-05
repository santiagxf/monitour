// Nv12ToTensor.hlsl
//
// One-pass compute shader: NV12 (BT.601 limited range) → planar RGB float16
// CHW tensor, resized to the model's expected square input (default 224x224),
// center-cropped, and normalized with ImageNet stats. Output layout matches
// 6DRepNet's input: [1, 3, H, W] float16.
//
// We read the camera frame as two textures (Y plane + UV plane) because
// NV12 is what Media Foundation hands us via the IMFDXGIDeviceManager path.

Texture2D<float>  YPlane  : register(t0);  // R8_UNORM,  width x height
Texture2D<float2> UVPlane : register(t1);  // R8G8_UNORM, (width/2) x (height/2)

RWStructuredBuffer<min16float> OutputTensor : register(u0);

cbuffer ResizeParams : register(b0) {
    uint  SrcWidth;      // camera frame width
    uint  SrcHeight;     // camera frame height
    uint  DstSize;       // square model input side (e.g. 224)
    uint  _pad0;
    float CropOriginX;   // source-pixel coords of top-left of the centered square crop
    float CropOriginY;
    float CropSize;      // side length in source pixels of the centered crop
    float _pad1;
}

static const float3 IMAGENET_MEAN = float3(0.485, 0.456, 0.406);
static const float3 IMAGENET_STD  = float3(0.229, 0.224, 0.225);

// BT.601 limited-range YUV → linear sRGB.
float3 Nv12ToRgb(float y, float2 uv) {
    float Y = (y - 16.0 / 255.0) * 1.164;
    float U = uv.x - 128.0 / 255.0;
    float V = uv.y - 128.0 / 255.0;
    float r = saturate(Y + 1.596 * V);
    float g = saturate(Y - 0.392 * U - 0.813 * V);
    float b = saturate(Y + 2.017 * U);
    return float3(r, g, b);
}

SamplerState BilinearClamp : register(s0);

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= DstSize || tid.y >= DstSize) {
        return;
    }

    // Map output pixel → source pixel coordinates via centered crop + bilinear resize.
    float u = (float(tid.x) + 0.5) / float(DstSize);
    float v = (float(tid.y) + 0.5) / float(DstSize);
    float srcX = (CropOriginX + u * CropSize) / float(SrcWidth);
    float srcY = (CropOriginY + v * CropSize) / float(SrcHeight);

    float  y  = YPlane .SampleLevel(BilinearClamp, float2(srcX, srcY), 0);
    float2 uv = UVPlane.SampleLevel(BilinearClamp, float2(srcX, srcY), 0);

    float3 rgb = Nv12ToRgb(y, uv);
    float3 normalized = (rgb - IMAGENET_MEAN) / IMAGENET_STD;

    // Write planar CHW: plane[c] is contiguous, stride = DstSize*DstSize.
    uint planeStride = DstSize * DstSize;
    uint pixelIdx    = tid.y * DstSize + tid.x;
    OutputTensor[0 * planeStride + pixelIdx] = (min16float)normalized.r;
    OutputTensor[1 * planeStride + pixelIdx] = (min16float)normalized.g;
    OutputTensor[2 * planeStride + pixelIdx] = (min16float)normalized.b;
}

// BGRA8 → NV12 color conversion compute shader.
//
// Input:   Texture2D<float4>  bgra source (read as DXGI_FORMAT_B8G8R8A8_UNORM)
// Output:  RWTexture2D<unorm float>   Y plane (DXGI_FORMAT_R8_UNORM)
//          RWTexture2D<unorm float2>  UV plane (DXGI_FORMAT_R8G8_UNORM, half-res)
//
// Uses BT.709 limited-range coefficients (standard for HD content). Override
// via the constant buffer if you need BT.601 or full-range later.
//
// Thread group size: 8x8 -> each thread writes 1 Y pixel + (potentially) 1 UV
// pixel. We write UV from the top-left thread of each 2x2 block.

cbuffer Params : register(b0)
{
    uint  g_width;
    uint  g_height;
    uint  g_full_range;   // 0 = limited (16-235), 1 = full (0-255)
    uint  _pad;
};

Texture2D<float4>          g_src        : register(t0);
RWTexture2D<float>         g_dst_y      : register(u0);
RWTexture2D<float2>        g_dst_uv     : register(u1);

static const float3 kBT709_Y  = float3(0.2126, 0.7152, 0.0722);
static const float3 kBT709_Cb = float3(-0.114572, -0.385428, 0.5);
static const float3 kBT709_Cr = float3(0.5, -0.454153, -0.045847);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_width || tid.y >= g_height) return;

    float4 bgra = g_src.Load(int3(tid.xy, 0));
    float3 rgb  = bgra.rgb;          // already gamma sRGB, NV12 stores gamma-encoded values

    float y_lin = dot(kBT709_Y,  rgb);

    if (g_full_range == 0) {
        // limited range: Y in [16, 235], UV in [16, 240]
        y_lin = y_lin * (219.0 / 255.0) + (16.0 / 255.0);
    }
    g_dst_y[tid.xy] = saturate(y_lin);

    // Write UV only from the top-left lane of each 2x2 block.
    if ((tid.x & 1u) == 0u && (tid.y & 1u) == 0u) {
        // Average a 2x2 block of source pixels for the chroma sample.
        float3 sum = rgb;
        if (tid.x + 1 < g_width)              sum += g_src.Load(int3(tid.x + 1, tid.y    , 0)).rgb;
        if (tid.y + 1 < g_height)             sum += g_src.Load(int3(tid.x    , tid.y + 1, 0)).rgb;
        if (tid.x + 1 < g_width && tid.y + 1 < g_height)
                                              sum += g_src.Load(int3(tid.x + 1, tid.y + 1, 0)).rgb;
        float3 avg = sum * 0.25;

        float cb = dot(kBT709_Cb, avg);
        float cr = dot(kBT709_Cr, avg);

        if (g_full_range == 0) {
            cb = cb * (224.0 / 255.0) + (128.0 / 255.0);
            cr = cr * (224.0 / 255.0) + (128.0 / 255.0);
        } else {
            cb += 0.5;
            cr += 0.5;
        }

        g_dst_uv[uint2(tid.x >> 1u, tid.y >> 1u)] = float2(saturate(cb), saturate(cr));
    }
}

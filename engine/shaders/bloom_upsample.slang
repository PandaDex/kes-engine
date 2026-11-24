// Bloom upsample: 3x3 tent filter of the smaller level, added (blend one/one)
// onto the next larger one.
#include "fullscreen.slang"

struct BloomConstants
{
    float4 params;   // xy = 1 / source size, z = filter radius in source texels
};

[[vk::push_constant]]
ConstantBuffer<BloomConstants> uBloom;   // b0 on D3D

[[vk::binding(0, 0)]]
Sampler2D uSource : register(t0);

float3 Tap(float2 uv, float2 offset)
{
    return uSource.SampleLevel(uv + offset * uBloom.params.xy * uBloom.params.z, 0.0).rgb;
}

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    float2 uv = input.uv;
    float3 color = Tap(uv, float2(0.0, 0.0)) * 4.0;
    color += (Tap(uv, float2(-1.0, 0.0)) + Tap(uv, float2(1.0, 0.0)) + Tap(uv, float2(0.0, -1.0)) + Tap(uv, float2(0.0, 1.0))) * 2.0;
    color += Tap(uv, float2(-1.0, -1.0)) + Tap(uv, float2(1.0, -1.0)) + Tap(uv, float2(-1.0, 1.0)) + Tap(uv, float2(1.0, 1.0));
    return float4(color * (1.0 / 16.0), 1.0);
}

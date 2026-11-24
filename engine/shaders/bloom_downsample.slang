// Bloom downsample (Jimenez, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare"): 13 bilinear taps, five overlapping 4-tap boxes. The first
// pass from the scene weights the boxes by 1 / (1 + luma) (Karis average) so
// single bright pixels don't flicker.
#include "fullscreen.slang"

struct BloomConstants
{
    float4 params;   // xy = 1 / source size, z = 1 first pass (Karis average)
};

[[vk::push_constant]]
ConstantBuffer<BloomConstants> uBloom;   // b0 on D3D

[[vk::binding(0, 0)]]
Sampler2D uSource : register(t0);

float3 Tap(float2 uv, float2 offset)
{
    // The clamp keeps inf and huge values from the scene out of the chain.
    return min(max(uSource.SampleLevel(uv + offset * uBloom.params.xy, 0.0).rgb, 0.0), 65000.0);
}

float KarisWeight(float3 c)
{
    return 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722)));
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
    float3 a = Tap(uv, float2(-2.0, -2.0));
    float3 b = Tap(uv, float2( 0.0, -2.0));
    float3 c = Tap(uv, float2( 2.0, -2.0));
    float3 d = Tap(uv, float2(-2.0,  0.0));
    float3 e = Tap(uv, float2( 0.0,  0.0));
    float3 f = Tap(uv, float2( 2.0,  0.0));
    float3 g = Tap(uv, float2(-2.0,  2.0));
    float3 h = Tap(uv, float2( 0.0,  2.0));
    float3 i = Tap(uv, float2( 2.0,  2.0));
    float3 j = Tap(uv, float2(-1.0, -1.0));
    float3 k = Tap(uv, float2( 1.0, -1.0));
    float3 l = Tap(uv, float2(-1.0,  1.0));
    float3 m = Tap(uv, float2( 1.0,  1.0));

    float3 box0 = (j + k + l + m) * 0.25;
    float3 box1 = (a + b + d + e) * 0.25;
    float3 box2 = (b + c + e + f) * 0.25;
    float3 box3 = (d + e + g + h) * 0.25;
    float3 box4 = (e + f + h + i) * 0.25;

    float w0 = 0.5;
    float w1 = 0.125;
    float w2 = 0.125;
    float w3 = 0.125;
    float w4 = 0.125;
    if (uBloom.params.z > 0.5)
    {
        w0 *= KarisWeight(box0);
        w1 *= KarisWeight(box1);
        w2 *= KarisWeight(box2);
        w3 *= KarisWeight(box3);
        w4 *= KarisWeight(box4);
    }
    float3 color = (box0 * w0 + box1 * w1 + box2 * w2 + box3 * w3 + box4 * w4) / (w0 + w1 + w2 + w3 + w4);
    return float4(color, 1.0);
}

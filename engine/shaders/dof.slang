// Depth of field: a single-pass golden-angle gather (after Dennis Gustafsson)
// over the HDR scene, using the prepass depth. Reads a copy of the scene color
// and writes the result back into it, before bloom and the tonemap.
#include "fullscreen.slang"
#include "kes_scene.slang"

[[vk::binding(0, 1)]] Texture2D<float> uPrepassDepth : register(t12);
[[vk::binding(2, 1)]] Texture2D<float4> uSourceColor : register(t14);
[[vk::binding(3, 1)]] SamplerState uSourceSampler : register(s0);

static const float kGoldenAngle = 2.39996323;
static const int kMaxSamples = 160;

float ViewDepthAt(float2 uv)
{
    int2 size = int2(uScreenParams.xy);
    int2 p = clamp(int2(uv * uScreenParams.xy), int2(0, 0), size - 1);
    return KesLinearDepth(uPrepassDepth.Load(int3(p, 0)));
}

// Blur radius in pixels.
float BlurSize(float viewDepth)
{
    float coc = saturate(abs(viewDepth - uDofParams.x) / max(uDofParams.y, 1e-3));
    return coc * uDofParams.z;
}

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    float2 uv = (floor(input.position.xy) + 0.5) * uScreenParams.zw;
    float3 color = uSourceColor.SampleLevel(uSourceSampler, uv, 0.0).rgb;
    float centerDepth = ViewDepthAt(uv);
    float centerSize = BlurSize(centerDepth);
    float maxSize = uDofParams.z;
    // Sample spacing grows with the blur so the count stays bounded.
    float radiusScale = max(1.0, maxSize * maxSize / (2.0 * float(kMaxSamples)));

    float total = 1.0;
    float radius = radiusScale;
    float angle = 0.0;
    for (int i = 0; i < kMaxSamples && radius < maxSize; i++)
    {
        float2 tc = uv + float2(cos(angle), sin(angle)) * uScreenParams.zw * radius;
        float3 sampleColor = uSourceColor.SampleLevel(uSourceSampler, tc, 0.0).rgb;
        float sampleDepth = ViewDepthAt(tc);
        float sampleSize = BlurSize(sampleDepth);
        // Sharp foreground over a blurred background doesn't bleed outwards.
        if (sampleDepth > centerDepth)
            sampleSize = clamp(sampleSize, 0.0, centerSize * 2.0);
        float m = smoothstep(radius - 0.5, radius + 0.5, sampleSize);
        color += lerp(color / total, sampleColor, m);
        total += 1.0;
        radius += radiusScale / radius;
        angle += kGoldenAngle;
    }
    return float4(color / total, 1.0);
}

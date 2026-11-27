// Temporal anti-aliasing on the HDR scene. The projection is jittered every
// frame (render_common ApplyTemporalJitter); this pass blends the frame into
// the reprojected history, clipped to the current 3x3 neighbourhood
// (variance clipping in YCoCg, in a reversible tonemapped space against
// fireflies). Camera motion only: the history is reprojected through the
// prepass depth with last frame's matrix, moving objects rely on the clip.
#include "fullscreen.slang"
#include "kes_scene.slang"

[[vk::binding(0, 1)]] Texture2D<float> uPrepassDepth : register(t12);
[[vk::binding(1, 1)]] Texture2D<float4> uTaaHistory : register(t13);
[[vk::binding(2, 1)]] Texture2D<float4> uCurrentColor : register(t14);
[[vk::binding(3, 1)]] SamplerState uLinearSampler : register(s0);

static const float kCurrentWeight = 0.1;

float Max3(float3 c)
{
    return max(c.r, max(c.g, c.b));
}

float3 Compress(float3 c)
{
    return c / (1.0 + Max3(c));
}

float3 Decompress(float3 c)
{
    return c / max(1.0 - Max3(c), 1e-4);
}

float3 RgbToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Bicubic Catmull-Rom from 9 bilinear taps (after Matt Pettineo).
float3 SampleHistory(float2 uv)
{
    float2 size = uScreenParams.xy;
    float2 samplePos = uv * size;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0 = (texPos1 - 1.0) * uScreenParams.zw;
    float2 tc3 = (texPos1 + 2.0) * uScreenParams.zw;
    float2 tc12 = (texPos1 + w2 / w12) * uScreenParams.zw;

    float3 result = float3(0.0, 0.0, 0.0);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc0.x, tc0.y), 0.0).rgb * (w0.x * w0.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc12.x, tc0.y), 0.0).rgb * (w12.x * w0.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc3.x, tc0.y), 0.0).rgb * (w3.x * w0.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc0.x, tc12.y), 0.0).rgb * (w0.x * w12.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc3.x, tc12.y), 0.0).rgb * (w3.x * w12.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc0.x, tc3.y), 0.0).rgb * (w0.x * w3.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc12.x, tc3.y), 0.0).rgb * (w12.x * w3.y);
    result += uTaaHistory.SampleLevel(uLinearSampler, float2(tc3.x, tc3.y), 0.0).rgb * (w3.x * w3.y);
    return max(result, 0.0);
}

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    int2 pixel = int2(input.position.xy);
    int2 size = int2(uScreenParams.xy);
    float3 current = max(uCurrentColor.Load(int3(pixel, 0)).rgb, 0.0);
    if (uTaaParams.w < 0.5)
        return float4(current, 1.0);

    // Neighbourhood statistics, and the nearest depth for the reprojection
    // (keeps foreground edges from picking up the background's motion).
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    float closestDepth = 1.0;
    int2 closestPixel = pixel;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            int2 p = clamp(pixel + int2(x, y), int2(0, 0), size - 1);
            float3 c = RgbToYCoCg(Compress(max(uCurrentColor.Load(int3(p, 0)).rgb, 0.0)));
            m1 += c;
            m2 += c * c;
            float d = uPrepassDepth.Load(int3(p, 0));
            if (d < closestDepth)
            {
                closestDepth = d;
                closestPixel = p;
            }
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float3 boxMin = mean - sigma * 1.25;
    float3 boxMax = mean + sigma * 1.25;

    float2 closestUV = (float2(closestPixel) + 0.5) * uScreenParams.zw;
    float3 position = KesWorldPositionFromDepth(closestUV, closestDepth);
    float4 prevClip = mul(float4(position, 1.0), uPrevViewProj);
    float2 prevNdc = prevClip.xy / max(prevClip.w, 1e-6);
    float2 uv = (float2(pixel) + 0.5) * uScreenParams.zw;
    float2 prevUV = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) + (uv - closestUV);
    if (prevClip.w <= 1e-6 || any(prevUV < 0.0) || any(prevUV > 1.0))
        return float4(current, 1.0);

    float3 history = RgbToYCoCg(Compress(SampleHistory(prevUV)));
    // Clip towards the neighbourhood mean instead of clamping per channel.
    float3 center = (boxMin + boxMax) * 0.5;
    float3 extent = max((boxMax - boxMin) * 0.5, 1e-5);
    float3 offset = history - center;
    float3 ratio = abs(offset / extent);
    float maxRatio = max(ratio.x, max(ratio.y, ratio.z));
    if (maxRatio > 1.0)
        history = center + offset / maxRatio;

    float3 currentY = RgbToYCoCg(Compress(current));
    float3 result = lerp(history, currentY, kCurrentWeight);
    return float4(Decompress(max(YCoCgToRgb(result), 0.0)), 1.0);
}

// Camera motion blur on the HDR scene: each pixel's screen motion since last
// frame (prepass depth reprojected with uPrevViewProj), blurred along that
// line. Moving objects don't blur on their own.
#include "fullscreen.slang"
#include "kes_scene.slang"

[[vk::binding(0, 1)]] Texture2D<float> uPrepassDepth : register(t12);
[[vk::binding(2, 1)]] Texture2D<float4> uSourceColor : register(t14);
[[vk::binding(3, 1)]] SamplerState uSourceSampler : register(s0);

static const int kSamples = 16;

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    int2 pixel = int2(input.position.xy);
    float2 uv = (float2(pixel) + 0.5) * uScreenParams.zw;
    float3 source = uSourceColor.SampleLevel(uSourceSampler, uv, 0.0).rgb;

    float depth = uPrepassDepth.Load(int3(pixel, 0));
    float3 position = KesWorldPositionFromDepth(uv, depth);
    float4 prevClip = mul(float4(position, 1.0), uPrevViewProj);
    if (prevClip.w <= 1e-6)
        return float4(source, 1.0);
    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUV = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
    // The current frame's jitter isn't motion.
    float2 velocity = (uv - uTaaParams.xy - prevUV) * uMotionBlurParams.x;
    float len = length(velocity);
    if (len > uMotionBlurParams.y)
        velocity *= uMotionBlurParams.y / len;
    if (length(velocity * uScreenParams.xy) < 0.5)
        return float4(source, 1.0);

    float3 sum = float3(0.0, 0.0, 0.0);
    for (int i = 0; i < kSamples; i++)
    {
        float t = (float(i) + 0.5) / float(kSamples) - 0.5;
        sum += uSourceColor.SampleLevel(uSourceSampler, uv + velocity * t, 0.0).rgb;
    }
    return float4(sum / float(kSamples), 1.0);
}

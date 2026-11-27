// Screen-space reflections from the depth + normal prepass. Rays march in
// world space against the prepass depth; a hit is reprojected into last
// frame's HDR scene color (uPrevViewProj). Output: radiance (rgb) and the
// weight the scene pass blends it over the environment reflection with (a).
#include "fullscreen.slang"
#include "kes_scene.slang"

[[vk::binding(0, 1)]] Texture2D<float> uPrepassDepth : register(t12);
[[vk::binding(1, 1)]] Texture2D<float4> uPrepassNormal : register(t13);
[[vk::binding(2, 1)]] Texture2D<float4> uHistoryColor : register(t14);
[[vk::binding(3, 1)]] SamplerState uHistorySampler : register(s0);

static const int kSteps = 32;
static const int kRefineSteps = 5;

float SceneDepthAt(float2 uv)
{
    int2 size = int2(uScreenParams.xy);
    int2 p = clamp(int2(uv * uScreenParams.xy), int2(0, 0), size - 1);
    return KesLinearDepth(uPrepassDepth.Load(int3(p, 0)));
}

float EdgeFade(float2 uv)
{
    float2 d = saturate((0.5 - abs(uv - 0.5)) * 10.0);
    return d.x * d.y;
}

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    if (!KesScreenReflectionsEnabled())
        return float4(0.0, 0.0, 0.0, 0.0);
    int2 pixel = int2(input.position.xy);
    float depth = uPrepassDepth.Load(int3(pixel, 0));
    if (depth >= 1.0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 normalRoughness = uPrepassNormal.Load(int3(pixel, 0));
    float roughness = normalRoughness.w;
    float maxRoughness = uSsrParams.y;
    if (roughness >= maxRoughness)
        return float4(0.0, 0.0, 0.0, 0.0);

    float2 uv = (float2(pixel) + 0.5) * uScreenParams.zw;
    float3 position = KesWorldPositionFromDepth(uv, depth);
    float3 n = normalize(normalRoughness.xyz);
    float3 v = normalize(position - uCameraPosition.xyz);
    float3 r = reflect(v, n);
    // Rays towards the camera leave the screen through its back.
    float facing = saturate(dot(r, uCameraForward.xyz) * 4.0 + 1.0);
    if (facing <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    float viewDepth = KesViewDepth(position);
    float maxDistance = uSsrParams.x;
    float3 origin = position + n * (0.02 + viewDepth * 0.002);
    float previousT = 0.0;
    float hitT = -1.0;
    for (int i = 1; i <= kSteps; i++)
    {
        float f = float(i) / float(kSteps);
        float t = maxDistance * f * f;
        float3 p = origin + r * t;
        float2 sampleUV = KesScreenUV(p);
        if (any(sampleUV < 0.0) || any(sampleUV > 1.0))
            break;
        float rayDepth = KesViewDepth(p);
        if (rayDepth <= 0.0)
            break;
        float delta = rayDepth - SceneDepthAt(sampleUV);
        float thickness = max(t - previousT, 0.1) + rayDepth * 0.01;
        if (delta > 0.0 && delta < thickness)
        {
            hitT = t;
            break;
        }
        previousT = t;
    }
    if (hitT < 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    float lo = previousT;
    float hi = hitT;
    for (int j = 0; j < kRefineSteps; j++)
    {
        float mid = (lo + hi) * 0.5;
        float3 p = origin + r * mid;
        if (KesViewDepth(p) > SceneDepthAt(KesScreenUV(p)))
            hi = mid;
        else
            lo = mid;
    }
    float2 hitUV = KesScreenUV(origin + r * hi);
    float hitDepth = uPrepassDepth.Load(int3(clamp(int2(hitUV * uScreenParams.xy), int2(0, 0), int2(uScreenParams.xy) - 1), 0));
    float3 hitPosition = KesWorldPositionFromDepth(hitUV, hitDepth);

    float4 prevClip = mul(float4(hitPosition, 1.0), uPrevViewProj);
    if (prevClip.w <= 1e-4)
        return float4(0.0, 0.0, 0.0, 0.0);
    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUV = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
    if (any(prevUV < 0.0) || any(prevUV > 1.0))
        return float4(0.0, 0.0, 0.0, 0.0);

    float3 color = max(uHistoryColor.SampleLevel(uHistorySampler, prevUV, 0.0).rgb, 0.0);
    float weight = EdgeFade(hitUV) * EdgeFade(prevUV) * facing * (1.0 - hi / maxDistance);
    weight *= 1.0 - smoothstep(maxRoughness * 0.5, maxRoughness, roughness);
    return float4(color, saturate(weight * uSsrParams.w));
}

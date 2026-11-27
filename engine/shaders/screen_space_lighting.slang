// Screen-space lighting from the depth + normal prepass, at the render
// resolution: ambient occlusion (normal-oriented hemisphere, 12 samples) in r
// and the contact shadow of one directional light (short ray march) in g.
// screen_space_lighting_blur.slang then filters it.
#include "fullscreen.slang"
#include "kes_scene.slang"

[[vk::binding(0, 1)]] Texture2D<float> uPrepassDepth : register(t12);
[[vk::binding(1, 1)]] Texture2D<float4> uPrepassNormal : register(t13);

static const int kAOSamples = 12;
static const int kContactSteps = 12;

float DepthAt(float2 uv)
{
    int2 size = int2(uScreenParams.xy);
    int2 p = clamp(int2(uv * uScreenParams.xy), int2(0, 0), size - 1);
    return uPrepassDepth.Load(int3(p, 0));
}

// 4x4 interleaved pattern; the blur pass averages it out.
float PatternNoise(int2 p)
{
    static const float kBayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return (kBayer[(p.y & 3) * 4 + (p.x & 3)] + 0.5) / 16.0;
}

float AmbientOcclusion(float3 position, float3 normal, float viewDepth, float noise)
{
    float radius = uSsaoParams.x;
    float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    float angle = noise * 6.2831853;
    float ca = cos(angle);
    float sa = sin(angle);
    float3 t = tangent * ca + bitangent * sa;
    float3 b = bitangent * ca - tangent * sa;

    float occlusion = 0.0;
    float bias = 0.02 + viewDepth * 0.002;
    for (int i = 0; i < kAOSamples; i++)
    {
        // Spiral over the hemisphere, denser near the centre.
        float fi = (float(i) + 0.5) / float(kAOSamples);
        float phi = float(i) * 2.3999632;
        float cosTheta = sqrt(1.0 - fi);
        float sinTheta = sqrt(fi);
        float3 dir = t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + normal * cosTheta;
        float scale = lerp(0.15, 1.0, frac(fi + noise) * frac(fi + noise));
        float3 samplePos = position + dir * (radius * scale);

        float2 uv = KesScreenUV(samplePos);
        if (any(uv < 0.0) || any(uv > 1.0))
            continue;
        float sceneDepth = KesLinearDepth(DepthAt(uv));
        float sampleDepth = KesViewDepth(samplePos);
        float range = saturate(radius / max(abs(viewDepth - sceneDepth), 1e-4));
        occlusion += (sceneDepth < sampleDepth - bias ? 1.0 : 0.0) * range * range;
    }
    float ao = 1.0 - occlusion / float(kAOSamples);
    return pow(saturate(ao), max(uSsaoParams.y, 0.0));
}

float ContactShadow(float3 position, float3 normal, float viewDepth, float noise)
{
    int index = int(uContactShadowParams.z);
    if (index < 0 || index >= int(uLightCounts.x))
        return 1.0;
    float3 toLight = -normalize(uLights[index].direction.xyz);
    if (dot(normal, toLight) <= 0.0)
        return 1.0; // self-shadowed anyway
    float len = uContactShadowParams.x;
    float thickness = uContactShadowParams.y * (1.0 + viewDepth * 0.02);
    float3 start = position + normal * (0.01 + viewDepth * 0.001);
    float stepLength = len / float(kContactSteps);
    for (int i = 0; i < kContactSteps; i++)
    {
        float3 p = start + toLight * (stepLength * (float(i) + noise));
        float2 uv = KesScreenUV(p);
        if (any(uv < 0.0) || any(uv > 1.0))
            break;
        float sceneDepth = KesLinearDepth(DepthAt(uv));
        float rayDepth = KesViewDepth(p);
        float delta = rayDepth - sceneDepth;
        if (delta > 0.0 && delta < thickness)
        {
            // Fade near the screen edge and the end of the ray.
            float2 edge = saturate((0.5 - abs(uv - 0.5)) * 20.0);
            return 1.0 - edge.x * edge.y * (1.0 - float(i) / float(kContactSteps));
        }
    }
    return 1.0;
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
    float depth = uPrepassDepth.Load(int3(pixel, 0));
    if (depth >= 1.0)
        return float4(1.0, 1.0, 0.0, 0.0);
    float2 uv = (float2(pixel) + 0.5) * uScreenParams.zw;
    float3 position = KesWorldPositionFromDepth(uv, depth);
    float3 normal = normalize(uPrepassNormal.Load(int3(pixel, 0)).xyz);
    float viewDepth = KesViewDepth(position);
    float noise = PatternNoise(pixel);

    float ao = uSsaoParams.z > 0.5 ? AmbientOcclusion(position, normal, viewDepth, noise) : 1.0;
    float contact = uSsaoParams.w > 0.5 ? ContactShadow(position, normal, viewDepth, noise) : 1.0;
    return float4(ao, contact, 0.0, 0.0);
}

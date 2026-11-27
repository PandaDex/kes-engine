// 4x4 depth-aware average of screen_space_lighting.slang (matches its 4x4 noise pattern).
#include "fullscreen.slang"
#include "kes_scene.slang"

[[vk::binding(0, 1)]] Texture2D<float> uPrepassDepth : register(t12);
[[vk::binding(1, 1)]] Texture2D<float4> uRawLighting : register(t13);

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
    float depth = uPrepassDepth.Load(int3(pixel, 0));
    if (depth >= 1.0)
        return float4(1.0, 1.0, 0.0, 0.0);
    float center = KesLinearDepth(depth);

    float2 sum = float2(0.0, 0.0);
    float weightSum = 0.0;
    for (int y = -2; y < 2; y++)
    {
        for (int x = -2; x < 2; x++)
        {
            int2 p = clamp(pixel + int2(x, y), int2(0, 0), size - 1);
            float d = KesLinearDepth(uPrepassDepth.Load(int3(p, 0)));
            float w = saturate(1.0 - abs(d - center) / (0.05 + center * 0.02));
            sum += uRawLighting.Load(int3(p, 0)).rg * w;
            weightSum += w;
        }
    }
    float2 result = weightSum > 1e-4 ? sum / weightSum : uRawLighting.Load(int3(pixel, 0)).rg;
    return float4(result, 0.0, 0.0);
}

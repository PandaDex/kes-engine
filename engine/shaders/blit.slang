// Tonemap: the HDR scene (resolved, linear) to the display. Exposure, ACES
// filmic curve (Hill's fit), sRGB encoding and a little dither against banding.
// The backbuffer stays UNORM, so the UI keeps drawing in display space.
#include "fullscreen.slang"

struct TonemapConstants
{
    float4 params;   // x = exposure, y = dither strength
};

[[vk::push_constant]]
ConstantBuffer<TonemapConstants> uTonemap;   // b0 on D3D

[[vk::binding(0, 0)]]
Sampler2D uSceneColor : register(t0);

static const float3x3 kAcesInput = float3x3(
    0.59719, 0.35458, 0.04823,
    0.07600, 0.90834, 0.01566,
    0.02840, 0.13383, 0.83777);

static const float3x3 kAcesOutput = float3x3(
     1.60475, -0.53108, -0.07367,
    -0.10208,  1.10813, -0.00605,
    -0.00327, -0.07276,  1.07602);

float3 AcesFitted(float3 color)
{
    color = mul(kAcesInput, color);
    float3 a = color * (color + 0.0245786) - 0.000090537;
    float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    color = mul(kAcesOutput, a / b);
    return saturate(color);
}

float3 LinearToSrgb(float3 c)
{
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return float3(c.r <= 0.0031308 ? low.r : high.r,
                  c.g <= 0.0031308 ? low.g : high.g,
                  c.b <= 0.0031308 ? low.b : high.b);
}

float Hash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    float3 hdr = max(uSceneColor.Sample(input.uv).rgb, 0.0) * uTonemap.params.x;
    float3 color = LinearToSrgb(AcesFitted(hdr));
    color += (Hash(input.position.xy) - 0.5) * uTonemap.params.y;
    return float4(color, 1.0);
}

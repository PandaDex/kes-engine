// Tonemap: the HDR scene (resolved, linear) to the display. Sharpening (CAS),
// bloom, exposure (manual times eye adaptation), vignette, colour grading, the
// selected tonemap curve, lift/gamma/gain, sRGB encoding and a little dither
// against banding. The backbuffer stays UNORM, so the UI keeps drawing in
// display space.
#include "fullscreen.slang"

struct TonemapConstants
{
    float4 params;          // x = exposure, y = dither strength, z = 1 auto exposure, w = bloom intensity
    float4 vignette;        // x = intensity, y = smoothness, z = aspect, w = 1 / bloom level count
    float4 vignetteColor;   // rgb, w = tonemapper (Tonemapper in scene.h)
    float4 whiteBalance;    // xyz = LMS scale, w = saturation
    float4 colorFilter;     // rgb, w = contrast
    float4 lift;            // rgb, w = sharpen strength
    float4 gamma;           // rgb = 1 / gamma, w = 1 / scene width
    float4 gain;            // rgb, w = 1 / scene height
};

[[vk::push_constant]]
ConstantBuffer<TonemapConstants> uTonemap;   // b0 on D3D

[[vk::binding(0, 0)]]
Sampler2D uSceneColor : register(t0);
[[vk::binding(1, 0)]]
Sampler2D uBloom : register(t1);
[[vk::binding(2, 0)]]
StructuredBuffer<float> uExposureState : register(t2);   // [0] = adapted exposure

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

static const float3x3 kLinearToLms = float3x3(
    3.90405e-1, 5.49941e-1, 8.92632e-3,
    7.08416e-2, 9.63172e-1, 1.35775e-3,
    2.31082e-2, 1.28021e-1, 9.36245e-1);

static const float3x3 kLmsToLinear = float3x3(
     2.85847e+0, -1.62879e+0, -2.48910e-2,
    -2.10182e-1,  1.15820e+0,  3.24281e-4,
    -4.18120e-2, -1.18169e-1,  1.06867e+0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

// AgX base (Troy Sobotka), polynomial fit by Benjamin Wrensch. Returns display linear.
float3 AgX(float3 color)
{
    static const float3x3 kAgxIn = float3x3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    static const float3x3 kAgxOut = float3x3(
         1.19687900512017, -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    static const float kMinEv = -12.47393;
    static const float kMaxEv = 4.026069;
    float3 x = mul(color, kAgxIn);
    x = clamp(log2(max(x, 1e-10)), kMinEv, kMaxEv);
    x = (x - kMinEv) / (kMaxEv - kMinEv);
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    x = mul(x, kAgxOut);
    return pow(saturate(x), 2.2);
}

float3 PbrNeutral(float3 color)
{
    static const float kStartCompression = 0.8 - 0.04;
    static const float kDesaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < kStartCompression)
        return saturate(color);
    float d = 1.0 - kStartCompression;
    float newPeak = 1.0 - d * d / (peak + d - kStartCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (kDesaturation * (peak - newPeak) + 1.0);
    return saturate(lerp(color, float3(newPeak, newPeak, newPeak), g));
}

float3 ApplyTonemapper(float3 color, int tonemapper)
{
    if (tonemapper == 1)
        return AgX(color);
    if (tonemapper == 2)
        return PbrNeutral(color);
    if (tonemapper == 3)
        return color / (1.0 + color);
    if (tonemapper == 4)
        return saturate(color);
    return AcesFitted(color);
}

float Max3(float3 c)
{
    return max(c.r, max(c.g, c.b));
}

// Reversible tonemap so sharpening works on HDR without ringing around highlights.
float3 SharpenEncode(float3 c)
{
    return c / (1.0 + Max3(c));
}

float3 SharpenDecode(float3 c)
{
    return c / max(1.0 - Max3(c), 1e-4);
}

float3 SampleExposed(float2 uv, float exposure)
{
    return SharpenEncode(max(uSceneColor.SampleLevel(uv, 0.0).rgb, 0.0) * exposure);
}

// AMD FidelityFX CAS, cross-shaped neighbourhood.
float3 SharpenedScene(float2 uv, float exposure)
{
    float2 texel = float2(uTonemap.gamma.w, uTonemap.gain.w);
    float3 c = SampleExposed(uv, exposure);
    float3 n = SampleExposed(uv + float2(0.0, -texel.y), exposure);
    float3 s = SampleExposed(uv + float2(0.0, texel.y), exposure);
    float3 w = SampleExposed(uv + float2(-texel.x, 0.0), exposure);
    float3 e = SampleExposed(uv + float2(texel.x, 0.0), exposure);
    float3 mn = min(c, min(min(n, s), min(w, e)));
    float3 mx = max(c, max(max(n, s), max(w, e)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
    float3 weight = amp * (-1.0 / lerp(8.0, 5.0, uTonemap.lift.w));
    float3 result = (c + (n + s + w + e) * weight) / (1.0 + 4.0 * weight);
    return SharpenDecode(saturate(result));
}

float3 GradeSceneLinear(float3 color)
{
    float3 lms = mul(kLinearToLms, color) * uTonemap.whiteBalance.xyz;
    color = max(mul(kLmsToLinear, lms), 0.0) * uTonemap.colorFilter.rgb;

    static const float kMiddleGrey = 0.18;
    float contrast = uTonemap.colorFilter.w;
    if (contrast != 1.0)
        color = kMiddleGrey * pow(max(color, 1e-6) / kMiddleGrey, contrast);

    float luma = dot(color, kLuma709);
    return max(lerp(float3(luma, luma, luma), color, uTonemap.whiteBalance.w), 0.0);
}

float3 LiftGammaGain(float3 color)
{
    color = color * uTonemap.gain.rgb + uTonemap.lift.rgb * (1.0 - color);
    return pow(saturate(color), uTonemap.gamma.rgb);
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
    float exposure = uTonemap.params.x;
    if (uTonemap.params.z > 0.5)
        exposure *= uExposureState[0];

    float3 hdr = uTonemap.lift.w > 0.0 ? SharpenedScene(input.uv, exposure)
                                       : max(uSceneColor.Sample(input.uv).rgb, 0.0) * exposure;
    if (uTonemap.params.w > 0.0)
        hdr = lerp(hdr, uBloom.SampleLevel(input.uv, 0.0).rgb * (uTonemap.vignette.w * exposure), uTonemap.params.w);

    if (uTonemap.vignette.x > 0.0)
    {
        float2 d = abs(input.uv - 0.5) * (uTonemap.vignette.x * 1.5);
        d.x *= uTonemap.vignette.z;
        float falloff = pow(saturate(1.0 - dot(d, d)), max(uTonemap.vignette.y * 5.0, 0.01));
        hdr *= lerp(uTonemap.vignetteColor.rgb, float3(1.0, 1.0, 1.0), falloff);
    }

    hdr = GradeSceneLinear(hdr);
    float3 color = LinearToSrgb(LiftGammaGain(ApplyTonemapper(hdr, int(uTonemap.vignetteColor.w + 0.5))));
    color += (Hash(input.position.xy) - 0.5) * uTonemap.params.y;
    return float4(color, 1.0);
}

// Final pass when post anti-aliasing is on: the tonemapped LDR image (sRGB
// encoded, render resolution) to the backbuffer. FXAA 3.11 quality (preset 29,
// after Timothy Lottes) or a plain copy after CMAA2.
#include "fullscreen.slang"

struct PresentConstants
{
    float4 params;   // xy = 1 / source size, z = 1 FXAA, 0 copy
};

[[vk::push_constant]]
ConstantBuffer<PresentConstants> uPresent;   // b0 on D3D

[[vk::binding(0, 0)]]
Sampler2D uSource : register(t0);

static const float kSubpixel = 0.75;
static const float kEdgeThreshold = 0.166;
static const float kEdgeThresholdMin = 0.0833;
static const int kSearchSteps = 12;
static const float kSearchStep[kSearchSteps] = { 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

float Luma(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

float LumaAt(float2 uv)
{
    return Luma(uSource.SampleLevel(uv, 0.0).rgb);
}

float LumaAt(float2 uv, float2 offset, float2 rcpFrame)
{
    return LumaAt(uv + offset * rcpFrame);
}

float3 Fxaa(float2 posM, float2 rcpFrame)
{
    float3 colorM = uSource.SampleLevel(posM, 0.0).rgb;
    float lumaM = Luma(colorM);
    float lumaS = LumaAt(posM, float2(0.0, 1.0), rcpFrame);
    float lumaE = LumaAt(posM, float2(1.0, 0.0), rcpFrame);
    float lumaN = LumaAt(posM, float2(0.0, -1.0), rcpFrame);
    float lumaW = LumaAt(posM, float2(-1.0, 0.0), rcpFrame);

    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold))
        return colorM;

    float lumaNW = LumaAt(posM, float2(-1.0, -1.0), rcpFrame);
    float lumaSE = LumaAt(posM, float2(1.0, 1.0), rcpFrame);
    float lumaNE = LumaAt(posM, float2(1.0, -1.0), rcpFrame);
    float lumaSW = LumaAt(posM, float2(-1.0, 1.0), rcpFrame);

    // Edge orientation from the 3x3 second derivatives.
    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(-2.0 * lumaM + lumaNS) * 2.0 + abs(-2.0 * lumaE + lumaNESE);
    float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(-2.0 * lumaM + lumaWE) * 2.0 + abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    float subpixA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    float subpixC = saturate(abs(subpixA * (1.0 / 12.0) - lumaM) / range);

    if (!horzSpan)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    float lengthSign = horzSpan ? rcpFrame.y : rcpFrame.x;
    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    float lumaNN = (pairN ? lumaN : lumaS) + lumaM;

    // Walk along the edge in both directions until the luma pair changes.
    float2 posB = posM;
    float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);
    if (horzSpan)
        posB.y += lengthSign * 0.5;
    else
        posB.x += lengthSign * 0.5;

    float gradientScaled = gradient * 0.25;
    float lumaHalf = lumaNN * 0.5;
    bool lumaMLTZero = lumaM - lumaHalf < 0.0;

    float2 posN = posB - offNP * kSearchStep[0];
    float2 posP = posB + offNP * kSearchStep[0];
    float lumaEndN = LumaAt(posN) - lumaHalf;
    float lumaEndP = LumaAt(posP) - lumaHalf;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;

    [loop]
    for (int i = 1; i < kSearchSteps && !(doneN && doneP); ++i)
    {
        if (!doneN)
        {
            posN -= offNP * kSearchStep[i];
            lumaEndN = LumaAt(posN) - lumaHalf;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP)
        {
            posP += offNP * kSearchStep[i];
            lumaEndP = LumaAt(posP) - lumaHalf;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }

    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool directionN = dstN < dstP;
    bool goodSpan = directionN ? ((lumaEndN < 0.0) != lumaMLTZero) : ((lumaEndP < 0.0) != lumaMLTZero);
    float pixelOffset = goodSpan ? 0.5 - min(dstN, dstP) / (dstN + dstP) : 0.0;

    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    float pixelOffsetSubpix = max(pixelOffset, subpixF * subpixF * kSubpixel);

    if (horzSpan)
        posM.y += pixelOffsetSubpix * lengthSign;
    else
        posM.x += pixelOffsetSubpix * lengthSign;
    return uSource.SampleLevel(posM, 0.0).rgb;
}

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    float3 color = uPresent.params.z > 0.5 ? Fxaa(input.uv, uPresent.params.xy)
                                           : uSource.SampleLevel(input.uv, 0.0).rgb;
    return float4(color, 1.0);
}

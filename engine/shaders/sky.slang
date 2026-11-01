// Procedural gradient sky with sun disc.
#include "fullscreen.slang"

[[vk::binding(0, 0)]]
cbuffer SkyCB : register(b0)
{
    float4 uTopColor;
    float4 uHorizonColor;
    float4 uBottomColor;
    float4 uSunColor;
    float4 uSunDirection;
    float4 uCameraForward;
    float4 uCameraRight;
    float4 uCameraUp;
    float4 uProjectionParams;
    float4 uSkyParams;
};

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float3 rayDir = normalize(
        uCameraForward.xyz +
        uCameraRight.xyz * (ndc.x * uProjectionParams.x) +
        uCameraUp.xyz * (ndc.y * uProjectionParams.y));

    float hemisphereBlend = pow(saturate(abs(rayDir.y)), max(uSkyParams.y, 0.01));
    float3 skyColor = rayDir.y >= 0.0
        ? lerp(uHorizonColor.rgb, uTopColor.rgb, hemisphereBlend)
        : lerp(uHorizonColor.rgb, uBottomColor.rgb, hemisphereBlend);

    float sunDot = dot(rayDir, normalize(-uSunDirection.xyz));
    float sunMask = smoothstep(uProjectionParams.z, uProjectionParams.w, sunDot);
    skyColor += uSunColor.rgb * (sunMask * uSkyParams.x);

    return float4(skyColor, 1.0);
}

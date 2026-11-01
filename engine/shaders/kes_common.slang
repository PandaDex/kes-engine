// KES shared shader declarations.
//
// Every mesh-family shader (engine built-ins and game material shaders) includes
// this file so the constant buffer layouts match the C++ side in every backend.
// Bindings are declared for both D3D (register) and Vulkan (vk::binding) so the
// same source compiles to DXBC and SPIR-V without per-backend edits.
//
// Matrices are row-major and multiplied as mul(vector, matrix).

#pragma once

struct GPULight
{
    float4 position;   // xyz = position, w = range
    float4 color;      // rgb = color, w = intensity
    float4 direction;  // xyz = direction, w = spot cos angle
    uint type;         // 0 = directional, 1 = point, 2 = spot
    uint lightActive;
    float2 padding;
};

[[vk::binding(0, 0)]]
cbuffer SceneCB : register(b0)
{
    row_major float4x4 uViewProj;
    float4 uAmbient;
    float4 uSnapParams;  // xy = snap grid, z = snap enabled
    GPULight uLights[8];
    float4 uFogColor;
    float4 uFogParams;   // x = start, y = end, z = enabled, w = steps
};

[[vk::binding(1, 0)]]
cbuffer ModelCB : register(b1)
{
    row_major float4x4 uWorldViewProj;
    row_major float4x4 uWorld;
    row_major float4x4 uSkinMatrices[128];
    float4 uSkinInfo;    // x = skinned
};

[[vk::binding(2, 0)]]
cbuffer MaterialCB : register(b2)
{
    float4 uBaseColor;
    float4 uEmissiveColorAndCutoff;  // rgb = emissive, a = alpha cutoff
    uint4 uMaterialFlags;            // x bits: 1 = lit, 8 = has base texture
};

static const uint kMaterialFlagLit = 1u;
static const uint kMaterialFlagHasTexture = 8u;

// Combined texture + sampler: set 1 binding 0 on Vulkan, t0/s0 on D3D.
[[vk::binding(0, 1)]]
Sampler2D uBaseTexture : register(t0);

struct MeshVertexInput
{
    [[vk::location(0)]] float3 position    : POSITION;
    [[vk::location(1)]] float3 normal      : NORMAL;
    [[vk::location(2)]] float2 uv          : TEXCOORD0;
    [[vk::location(3)]] float4 color       : COLOR;
    [[vk::location(4)]] uint4  boneIndices : BLENDINDICES;
    [[vk::location(5)]] float4 boneWeights : BLENDWEIGHT;
};

struct MeshVertexResult
{
    float4 clipPosition;
    float4 color;
    float2 uv;
};

// Skinning + per-vertex PSX lighting + optional vertex snapping.
MeshVertexResult KesShadeMeshVertex(MeshVertexInput input)
{
    float4 localPos = float4(input.position, 1.0);
    float3 localNrm = input.normal;
    if (uSkinInfo.x > 0.5)
    {
        float4x4 m0 = uSkinMatrices[min(input.boneIndices.x, 127u)];
        float4x4 m1 = uSkinMatrices[min(input.boneIndices.y, 127u)];
        float4x4 m2 = uSkinMatrices[min(input.boneIndices.z, 127u)];
        float4x4 m3 = uSkinMatrices[min(input.boneIndices.w, 127u)];

        localPos =
            mul(localPos, m0) * input.boneWeights.x +
            mul(localPos, m1) * input.boneWeights.y +
            mul(localPos, m2) * input.boneWeights.z +
            mul(localPos, m3) * input.boneWeights.w;
        localNrm =
            mul(float4(localNrm, 0.0), m0).xyz * input.boneWeights.x +
            mul(float4(localNrm, 0.0), m1).xyz * input.boneWeights.y +
            mul(float4(localNrm, 0.0), m2).xyz * input.boneWeights.z +
            mul(float4(localNrm, 0.0), m3).xyz * input.boneWeights.w;
    }

    float4 worldPos = mul(localPos, uWorld);
    float3 worldNrm = normalize(mul(float4(localNrm, 0.0), uWorld).xyz);
    bool materialLit = (uMaterialFlags.x & kMaterialFlagLit) != 0u;
    float3 lighting = materialLit ? uAmbient.rgb : float3(1.0, 1.0, 1.0);

    for (int i = 0; i < 8; i++)
    {
        if (!materialLit)
            break;
        if (uLights[i].lightActive == 0u)
            continue;

        float3 lightColor = uLights[i].color.rgb * uLights[i].color.w;
        float3 L = float3(0.0, 0.0, 0.0);
        float attenuation = 1.0;

        if (uLights[i].type == 0u)
        {
            L = normalize(-uLights[i].direction.xyz);
        }
        else
        {
            float3 toLight = uLights[i].position.xyz - worldPos.xyz;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            float range = uLights[i].position.w;
            attenuation = saturate(1.0 - dist / max(range, 0.0001));

            if (uLights[i].type == 2u)
            {
                float3 lightDir = normalize(uLights[i].direction.xyz);
                float cosAngle = dot(-L, lightDir);
                float spotAngle = uLights[i].direction.w;
                float spotAtten = saturate((cosAngle - spotAngle) / (1.0 - spotAngle));
                attenuation *= spotAtten;
            }
        }

        float ndl = max(dot(worldNrm, L), 0.0);
        lighting += ndl * lightColor * attenuation;
    }

    MeshVertexResult result;
    result.color = float4(input.color.rgb * uBaseColor.rgb * lighting + uEmissiveColorAndCutoff.rgb,
                          input.color.a * uBaseColor.a);
    result.uv = input.uv;

    float4 clip = mul(localPos, uWorldViewProj);
    if (uSnapParams.z > 0.5 &&
        clip.w > 0.0001 &&
        clip.z >= 0.0 &&
        clip.z <= clip.w &&
        abs(clip.x) <= clip.w &&
        abs(clip.y) <= clip.w)
    {
        float2 ndc = clip.xy / clip.w;
        ndc.x = floor(ndc.x * uSnapParams.x) / uSnapParams.x;
        ndc.y = floor(ndc.y * uSnapParams.y) / uSnapParams.y;
        clip.xy = ndc * clip.w;
    }

    result.clipPosition = clip;
    return result;
}

// Base texture * vertex color with alpha cutoff (discards).
float4 KesShadeMeshPixel(float4 vertexColor, float2 uv)
{
    float4 texel = (uMaterialFlags.x & kMaterialFlagHasTexture) != 0u
        ? uBaseTexture.Sample(uv)
        : float4(1.0, 1.0, 1.0, 1.0);
    float4 color = vertexColor * texel;
    if (color.a < uEmissiveColorAndCutoff.a)
        discard;
    return color;
}

// Stepped distance fog driven by SceneCB.uFogParams.
float3 KesApplyFog(float3 color, float fogDepth)
{
    if (uFogParams.z > 0.5)
    {
        float fogRange = max(uFogParams.y - uFogParams.x, 0.0001);
        float fogAmount = saturate((fogDepth - uFogParams.x) / fogRange);
        float fogSteps = max(uFogParams.w, 1.0);
        if (fogSteps > 1.0)
            fogAmount = floor(fogAmount * fogSteps + 0.0001) / fogSteps;
        color = lerp(color, uFogColor.rgb, fogAmount);
    }
    return color;
}

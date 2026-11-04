// KES shared shader declarations and PBR shading.
//
// Every mesh-family shader (engine built-ins and game material shaders) includes
// this file so the constant buffer layouts match the C++ side in every backend
// (render::SceneCBData etc., engine/render_common.h). Bindings are declared for
// both D3D (register) and Vulkan (vk::binding) so the same source compiles to
// DXBC and SPIR-V without per-backend edits.
//
//   set 0  b0 SceneCB, b1 ModelCB, b2 MaterialCB
//   set 1  t0-t4 material textures (base, normal, metallicRoughness, occlusion, emissive)
//   set 2  t5 environment cube, t6 sun cascades, t7 local shadows, s5 linear clamp, s6 shadow compare
//
// Matrices are row-major and multiplied as mul(vector, matrix). Lighting is in
// linear HDR; the tonemap pass (blit.slang) maps it to the display.

#pragma once

static const int kMaxLights = 32;
static const int kMaxCascades = 4;
static const int kMaxLocalShadowSlices = 16;
static const float kPi = 3.14159265;

struct GPULight
{
    float4 position;   // xyz, w = range
    float4 color;      // rgb = radiance
    float4 direction;  // xyz = direction the light travels, w = spot outer cos
    float4 params;     // x = type (0 dir, 1 point, 2 spot), y = spot inner cos, z = shadow (dir: 1 = cascades, local: first slice, -1 none)
};

[[vk::binding(0, 0)]]
cbuffer SceneCB : register(b0)
{
    row_major float4x4 uViewProj;
    float4 uCameraPosition;
    float4 uCameraForward;
    float4 uAmbientSH[9];          // irradiance / pi
    float4 uEnvironmentParams;     // x = specular mip count, y = intensity, z = time in seconds
    float4 uLightCounts;           // x = light count
    GPULight uLights[kMaxLights];
    float4 uFogColor;
    float4 uFogParams;             // x = start, y = end, z = enabled, w = max density
    row_major float4x4 uCascadeViewProj[kMaxCascades];
    float4 uCascadeSplits;
    float4 uCascadeTexelSize;
    float4 uShadowParams;          // x = cascade count, y = 1 / cascade res, z = 1 / local res, w = enabled
    row_major float4x4 uLocalShadowViewProj[kMaxLocalShadowSlices];
};

[[vk::binding(1, 0)]]
cbuffer ModelCB : register(b1)
{
    row_major float4x4 uWorldViewProj;
    row_major float4x4 uWorld;
    float4 uSkinInfo;    // x = skinned
    row_major float4x4 uSkinMatrices[128];
};

[[vk::binding(2, 0)]]
cbuffer MaterialCB : register(b2)
{
    float4 uBaseColor;
    float4 uEmissive;          // rgb = emissive, a = alpha cutoff
    float4 uPbr;               // x = metallic, y = roughness, z = normal scale, w = occlusion strength
    uint4 uMaterialFlags;      // x = MaterialFlags (material.h)
};

static const uint kMaterialFlagLit = 1u;
static const uint kMaterialFlagDoubleSided = 4u;
static const uint kMaterialFlagHasBase = 8u;
static const uint kMaterialFlagHasNormal = 16u;
static const uint kMaterialFlagHasMetallicRoughness = 32u;
static const uint kMaterialFlagHasOcclusion = 64u;
static const uint kMaterialFlagHasEmissive = 128u;

float KesTime()
{
    return uEnvironmentParams.z;
}

bool KesMaterialHas(uint flag)
{
    return (uMaterialFlags.x & flag) != 0u;
}

// Material textures: combined texture + sampler (set 1 on Vulkan, t0-t4/s0-s4 on D3D).
[[vk::binding(0, 1)]] Sampler2D uBaseTexture : register(t0);
[[vk::binding(1, 1)]] Sampler2D uNormalTexture : register(t1);
[[vk::binding(2, 1)]] Sampler2D uMetallicRoughnessTexture : register(t2);
[[vk::binding(3, 1)]] Sampler2D uOcclusionTexture : register(t3);
[[vk::binding(4, 1)]] Sampler2D uEmissiveTexture : register(t4);

// Frame-global resources (set 2 on Vulkan).
[[vk::binding(0, 2)]] TextureCube<float4> uEnvironmentMap : register(t5);
[[vk::binding(1, 2)]] Texture2DArray<float> uCascadeShadowMap : register(t6);
[[vk::binding(2, 2)]] Texture2DArray<float> uLocalShadowMap : register(t7);
[[vk::binding(3, 2)]] SamplerState uLinearClampSampler : register(s5);
[[vk::binding(4, 2)]] SamplerComparisonState uShadowSampler : register(s6);

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
    float3 worldPosition;
    float3 worldNormal;
    float4 color;
    float2 uv;
};

// Skinning and transforms. clipPosition uses ModelCB.uWorldViewProj, which the
// renderer sets to world * (camera or shadow) view-projection.
MeshVertexResult KesMeshVertex(MeshVertexInput input)
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

    MeshVertexResult result;
    result.worldPosition = mul(localPos, uWorld).xyz;
    result.worldNormal = mul(float4(localNrm, 0.0), uWorld).xyz;
    result.clipPosition = mul(localPos, uWorldViewProj);
    result.color = input.color;
    result.uv = input.uv;
    return result;
}

// BRDF (GGX / Smith height-correlated / Schlick)

float KesDistributionGGX(float NoH, float a)
{
    float a2 = a * a;
    float d = (NoH * a2 - NoH) * NoH + 1.0;
    return a2 / (kPi * d * d + 1e-7);
}

float KesVisibilitySmithGGX(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float v = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float l = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / (v + l + 1e-7);
}

float3 KesFresnelSchlick(float3 f0, float VoH)
{
    float f = pow(1.0 - VoH, 5.0);
    return f0 + (1.0 - f0) * f;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
float3 KesEnvironmentBRDF(float3 f0, float roughness, float NoV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float3 KesAmbientIrradiance(float3 n)
{
    float3 result = uAmbientSH[0].rgb * 0.282095;
    result += uAmbientSH[1].rgb * (0.488603 * n.y);
    result += uAmbientSH[2].rgb * (0.488603 * n.z);
    result += uAmbientSH[3].rgb * (0.488603 * n.x);
    result += uAmbientSH[4].rgb * (1.092548 * n.x * n.y);
    result += uAmbientSH[5].rgb * (1.092548 * n.y * n.z);
    result += uAmbientSH[6].rgb * (0.315392 * (3.0 * n.z * n.z - 1.0));
    result += uAmbientSH[7].rgb * (1.092548 * n.x * n.z);
    result += uAmbientSH[8].rgb * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(result, 0.0);
}

// Shadows: 3x3 hardware PCF taps.

float KesSampleShadowPCF(Texture2DArray<float> map, float2 uv, float slice, float depth, float texel)
{
    float sum = 0.0;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
            sum += map.SampleCmpLevelZero(uShadowSampler, float3(uv + float2(x, y) * texel, slice), depth);
    }
    return sum / 9.0;
}

float2 KesShadowUV(float2 ndc)
{
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float KesCascadeShadow(float3 worldPos, float3 geometricNormal)
{
    int count = int(uShadowParams.x);
    float viewDepth = dot(worldPos - uCameraPosition.xyz, uCameraForward.xyz);
    int cascade = count;
    for (int i = 0; i < count; i++)
    {
        if (viewDepth < uCascadeSplits[i])
        {
            cascade = i;
            break;
        }
    }
    if (cascade >= count)
        return 1.0;

    float3 p = worldPos + geometricNormal * (uCascadeTexelSize[cascade] * 1.5);
    float4 clip = mul(float4(p, 1.0), uCascadeViewProj[cascade]);
    float2 uv = KesShadowUV(clip.xy);
    if (any(uv < 0.0) || any(uv > 1.0) || clip.z > 1.0)
        return 1.0;
    return KesSampleShadowPCF(uCascadeShadowMap, uv, float(cascade), clip.z - 0.0005, uShadowParams.y);
}

float KesLocalShadow(GPULight light, float3 worldPos, float3 geometricNormal)
{
    float slice = light.params.z;
    float3 fromLight = worldPos - light.position.xyz;
    if (light.params.x < 1.5)
    {
        // Point light: one slice per cube face, picked by the major axis
        // (+X -X +Y -Y +Z -Z, render_common.cpp).
        float3 a = abs(fromLight);
        float face = a.x >= a.y && a.x >= a.z ? (fromLight.x >= 0.0 ? 0.0 : 1.0)
                   : a.y >= a.z ? (fromLight.y >= 0.0 ? 2.0 : 3.0)
                   : (fromLight.z >= 0.0 ? 4.0 : 5.0);
        slice += face;
    }
    int index = int(slice);
    float distanceToLight = length(fromLight);
    float3 p = worldPos + geometricNormal * (distanceToLight * uShadowParams.z * 2.0);
    float4 clip = mul(float4(p, 1.0), uLocalShadowViewProj[index]);
    if (clip.w <= 0.0001)
        return 1.0;
    float3 ndc = clip.xyz / clip.w;
    float2 uv = KesShadowUV(ndc.xy);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
        return 1.0;
    return KesSampleShadowPCF(uLocalShadowMap, uv, slice, ndc.z - 0.00005, uShadowParams.z);
}

// Normal mapping without vertex tangents: cotangent frame from screen-space
// derivatives (Schueler, "Normal Mapping Without Precomputed Tangents").
float3 KesPerturbNormal(float3 n, float3 worldPos, float2 uv, float3 tangentNormal)
{
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, n);
    float3 dp1perp = cross(n, dp1);
    float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invMax = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-12));
    return normalize(t * (tangentNormal.x * invMax) + b * (tangentNormal.y * invMax) + n * tangentNormal.z);
}

float3 KesApplyFog(float3 color, float3 worldPos)
{
    if (uFogParams.z > 0.5)
    {
        float distanceToCamera = length(worldPos - uCameraPosition.xyz);
        float amount = saturate((distanceToCamera - uFogParams.x) / max(uFogParams.y - uFogParams.x, 0.0001)) * uFogParams.w;
        color = lerp(color, uFogColor.rgb, amount);
    }
    return color;
}

struct SurfaceInput
{
    float3 worldPosition;
    float3 worldNormal;
    float4 vertexColor;
    float2 uv;
    bool frontFace;
};

// Inputs of the lighting model. KesMaterialSurface fills it from the .mat values
// and textures; a material shader can override fields before KesShadeMaterial.
struct KesSurface
{
    float3 baseColor;
    float alpha;
    float3 emissive;
    float3 normal;     // world space
    float metallic;
    float roughness;
    float occlusion;
};

float3 KesGeometricNormal(SurfaceInput s)
{
    float3 n = normalize(s.worldNormal);
    return !s.frontFace && KesMaterialHas(kMaterialFlagDoubleSided) ? -n : n;
}

float3 KesTangentNormal(SurfaceInput s, float3 tangentNormal)
{
    return KesPerturbNormal(KesGeometricNormal(s), s.worldPosition, s.uv, normalize(tangentNormal));
}

KesSurface KesMaterialSurface(SurfaceInput s)
{
    float4 baseColor = uBaseColor * s.vertexColor;
    if (KesMaterialHas(kMaterialFlagHasBase))
        baseColor *= uBaseTexture.Sample(s.uv);

    KesSurface m;
    m.baseColor = baseColor.rgb;
    m.alpha = baseColor.a;
    m.emissive = uEmissive.rgb;
    if (KesMaterialHas(kMaterialFlagHasEmissive))
        m.emissive *= uEmissiveTexture.Sample(s.uv).rgb;
    m.normal = KesGeometricNormal(s);
    m.metallic = uPbr.x;
    m.roughness = uPbr.y;
    m.occlusion = 1.0;

    if (KesMaterialHas(kMaterialFlagLit))
    {
        if (KesMaterialHas(kMaterialFlagHasNormal))
        {
            float3 tangentNormal = uNormalTexture.Sample(s.uv).xyz * 2.0 - 1.0;
            tangentNormal.xy *= uPbr.z;
            m.normal = KesTangentNormal(s, tangentNormal);
        }
        if (KesMaterialHas(kMaterialFlagHasMetallicRoughness))
        {
            float4 mr = uMetallicRoughnessTexture.Sample(s.uv);
            m.roughness *= mr.g;
            m.metallic *= mr.b;
        }
        if (KesMaterialHas(kMaterialFlagHasOcclusion))
            m.occlusion = lerp(1.0, uOcclusionTexture.Sample(s.uv).r, uPbr.w);
    }
    return m;
}

// Direct lights with shadows, image-based ambient, emissive and fog. Returns
// linear HDR color with alpha. Discards below the alpha cutoff.
float4 KesShadeMaterial(SurfaceInput s, KesSurface m)
{
    if (uEmissive.a > 0.0 && m.alpha < uEmissive.a)
        discard;

    if (!KesMaterialHas(kMaterialFlagLit))
        return float4(KesApplyFog(m.baseColor + m.emissive, s.worldPosition), m.alpha);

    float3 geometricNormal = KesGeometricNormal(s);
    float3 n = m.normal;
    float metallic = saturate(m.metallic);
    float roughness = clamp(m.roughness, 0.03, 1.0);
    float occlusion = m.occlusion;

    float3 v = normalize(uCameraPosition.xyz - s.worldPosition);
    float NoV = max(dot(n, v), 1e-4);
    float a = roughness * roughness;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), m.baseColor, metallic);
    float3 diffuseColor = m.baseColor * (1.0 - metallic);

    float3 direct = float3(0.0, 0.0, 0.0);
    int lightCount = int(uLightCounts.x);
    for (int i = 0; i < lightCount; i++)
    {
        GPULight light = uLights[i];
        float3 l;
        float attenuation = 1.0;
        if (light.params.x < 0.5)
        {
            l = -normalize(light.direction.xyz);
        }
        else
        {
            float3 toLight = light.position.xyz - s.worldPosition;
            float dist = length(toLight);
            l = toLight / max(dist, 0.0001);
            float x = dist / light.position.w;
            float window = saturate(1.0 - x * x * x * x);
            attenuation = window * window / (1.0 + 4.0 * x * x);
            if (light.params.x > 1.5)
                attenuation *= smoothstep(light.direction.w, light.params.y, dot(-l, normalize(light.direction.xyz)));
        }

        float NoL = dot(n, l);
        if (NoL <= 0.0 || attenuation <= 0.0)
            continue;

        float shadow = 1.0;
        if (uShadowParams.w > 0.5 && light.params.z >= 0.0)
        {
            if (light.params.x < 0.5)
                shadow = KesCascadeShadow(s.worldPosition, geometricNormal);
            else
                shadow = KesLocalShadow(light, s.worldPosition, geometricNormal);
        }

        float3 h = normalize(v + l);
        float NoH = saturate(dot(n, h));
        float VoH = saturate(dot(v, h));
        float3 f = KesFresnelSchlick(f0, VoH);
        float3 specular = KesDistributionGGX(NoH, a) * KesVisibilitySmithGGX(NoV, NoL, a) * f;
        float3 diffuse = (1.0 - f) * diffuseColor / kPi;
        direct += (diffuse + specular) * light.color.rgb * (NoL * attenuation * shadow);
    }

    // Image-based ambient: SH irradiance for diffuse, the prefiltered sky cube for reflections.
    float3 r = reflect(-v, n);
    float mip = roughness * (uEnvironmentParams.x - 1.0);
    float3 prefiltered = uEnvironmentMap.SampleLevel(uLinearClampSampler, r, mip).rgb;
    float3 ambient = (diffuseColor * KesAmbientIrradiance(n) + prefiltered * KesEnvironmentBRDF(f0, roughness, NoV)) *
                     (occlusion * uEnvironmentParams.y);

    float3 color = direct + ambient + m.emissive;
    return float4(KesApplyFog(color, s.worldPosition), m.alpha);
}

// Full material as authored in the .mat file.
float4 KesShadeSurface(SurfaceInput s)
{
    return KesShadeMaterial(s, KesMaterialSurface(s));
}

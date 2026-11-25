// Particle sprites and ribbons (render::ParticleVertex): world-space vertices
// built on the CPU, color = particle color (HDR) * the material's base color
// and base texture.
//
// ModelCB.uSkinInfo: x = shape without a base texture (1 soft disc for
// sprites, 2 soft edges across ribbons, 0 none), y = 1 lit (ambient + lights on a wrapped facing normal),
// z = 1 output premultiplied color (additive and premultiplied blending),
// w = 1 additive (fog fades the particle out instead of tinting it).
#include "kes_common.slang"

struct ParticleVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv       : TEXCOORD0;
    [[vk::location(2)]] float4 color    : COLOR;
    [[vk::location(3)]] float3 normal   : NORMAL;
};

struct ParticleVSOutput
{
    float4 position      : SV_Position;
    float3 worldPosition : TEXCOORD1;
    float3 normal        : NORMAL;
    float4 color         : COLOR0;
    float2 uv            : TEXCOORD0;
};

[shader("vertex")]
ParticleVSOutput vsMain(ParticleVertexInput input)
{
    ParticleVSOutput o;
    o.position = mul(float4(input.position, 1.0), uViewProj);
    o.worldPosition = input.position;
    o.normal = input.normal;
    o.color = input.color;
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 psMain(ParticleVSOutput input) : SV_Target
{
    float4 color = input.color * uBaseColor;
    if (KesMaterialHas(kMaterialFlagHasBase))
        color *= uBaseTexture.Sample(input.uv);
    else if (uSkinInfo.x > 1.5)
        color.a *= saturate(1.0 - abs(input.uv.y * 2.0 - 1.0)) * 1.5;
    else if (uSkinInfo.x > 0.5)
    {
        float r = saturate(1.0 - length(input.uv * 2.0 - 1.0));
        color.a *= r * r;
    }

    if (uSkinInfo.y > 0.5)
        color.rgb *= KesParticleLighting(input.worldPosition, input.normal);
    color.rgb += uEmissive.rgb * color.a;

    if (uFogParams.z > 0.5)
    {
        float distanceToCamera = length(input.worldPosition - uCameraPosition.xyz);
        float amount = saturate((distanceToCamera - uFogParams.x) / max(uFogParams.y - uFogParams.x, 0.0001)) * uFogParams.w;
        if (uSkinInfo.w > 0.5)
            color.rgb *= 1.0 - amount;
        else
            color.rgb = lerp(color.rgb, uFogColor.rgb, amount);
    }

    color.a = saturate(color.a);
    if (uSkinInfo.z > 0.5)
        color.rgb *= color.a;
    return color;
}

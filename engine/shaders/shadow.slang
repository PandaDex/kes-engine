// Shadow map depth pass. ModelCB.uWorldViewProj holds world * light view-projection.
// Alpha-tested materials (AlphaCutoff > 0) cut holes into their shadows.
#include "kes_common.slang"

struct ShadowVSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float alpha     : TEXCOORD1;
};

[shader("vertex")]
ShadowVSOutput vsMain(MeshVertexInput input)
{
    MeshVertexResult v = KesMeshVertex(input);
    ShadowVSOutput o;
    o.position = v.clipPosition;
    o.uv = v.uv;
    o.alpha = v.color.a * uBaseColor.a;
    return o;
}

[shader("fragment")]
void psMain(ShadowVSOutput input)
{
    if (uEmissive.a > 0.0)
    {
        float alpha = input.alpha;
        if (KesMaterialHas(kMaterialFlagHasBase))
            alpha *= uBaseTexture.Sample(input.uv).a;
        if (alpha < uEmissive.a)
            discard;
    }
}

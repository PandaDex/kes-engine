// Fog-capable mesh program. Used for materials that reference a .shader asset
// without providing their own stage, and as the reference template for game
// material shaders (copy this file into your content folder to start one).
#include "kes_common.slang"

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
    float fogDepth  : TEXCOORD1;
};

[shader("vertex")]
VSOutput vsMain(MeshVertexInput input)
{
    MeshVertexResult v = KesShadeMeshVertex(input);
    VSOutput o;
    o.position = v.clipPosition;
    o.color = v.color;
    o.uv = v.uv;
    o.fogDepth = max(v.clipPosition.w, 0.0);
    return o;
}

[shader("fragment")]
float4 psMain(VSOutput input) : SV_Target
{
    float4 color = KesShadeMeshPixel(input.color, input.uv);
    color.rgb = KesApplyFog(color.rgb, input.fogDepth);
    return color;
}

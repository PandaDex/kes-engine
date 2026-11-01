// Default always-resident mesh program (no fog).
#include "kes_common.slang"

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain(MeshVertexInput input)
{
    MeshVertexResult v = KesShadeMeshVertex(input);
    VSOutput o;
    o.position = v.clipPosition;
    o.color = v.color;
    o.uv = v.uv;
    return o;
}

[shader("fragment")]
float4 psMain(VSOutput input) : SV_Target
{
    return KesShadeMeshPixel(input.color, input.uv);
}

// Reference template for game material shaders (.shader assets): copy this
// file into your content folder to start one, then change what the surface
// receives or post-process the shaded color. Stages a .shader asset doesn't
// provide also fall back to this program.
//
// Building blocks (kes_common.slang):
//   KesMeshVertex      skinning + transforms
//   KesShadeSurface    PBR lights, shadows, image-based ambient, emissive, fog
//   KesApplyFog, KesAmbientIrradiance, KesEnvironmentBRDF, ...
#include "kes_common.slang"

struct VSOutput
{
    float4 position      : SV_Position;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal   : NORMAL;
    float4 color         : COLOR0;
    float2 uv            : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain(MeshVertexInput input)
{
    MeshVertexResult v = KesMeshVertex(input);
    VSOutput o;
    o.position = v.clipPosition;
    o.worldPosition = v.worldPosition;
    o.worldNormal = v.worldNormal;
    o.color = v.color;
    o.uv = v.uv;
    return o;
}

[shader("fragment")]
float4 psMain(VSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    SurfaceInput s;
    s.worldPosition = input.worldPosition;
    s.worldNormal = input.worldNormal;
    s.vertexColor = input.color;
    s.uv = input.uv;
    s.frontFace = isFrontFace;
    return KesShadeSurface(s);
}

// Depth + normal prepass of the opaque meshes for the screen-space passes.
// Output: world normal (xyz) and roughness (w). Uses the .mat surface;
// material shaders' own surface changes aren't seen here.
#include "kes_common.slang"

struct DepthNormalVSOutput
{
    float4 position      : SV_Position;
    float3 worldPosition : TEXCOORD1;
    float3 worldNormal   : NORMAL;
    float4 color         : COLOR0;
    float2 uv            : TEXCOORD0;
};

[shader("vertex")]
DepthNormalVSOutput vsMain(MeshVertexInput input)
{
    MeshVertexResult v = KesMeshVertex(input);
    DepthNormalVSOutput o;
    o.position = v.clipPosition;
    o.worldPosition = v.worldPosition;
    o.worldNormal = v.worldNormal;
    o.color = v.color;
    o.uv = v.uv;
    return o;
}

[shader("fragment")]
float4 psMain(DepthNormalVSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    SurfaceInput s;
    s.worldPosition = input.worldPosition;
    s.worldNormal = input.worldNormal;
    s.vertexColor = input.color;
    s.uv = input.uv;
    s.frontFace = isFrontFace;
    KesSurface m = KesMaterialSurface(s);
    if (uEmissive.a > 0.0 && m.alpha < uEmissive.a)
        discard;
    return float4(normalize(m.normal), saturate(m.roughness));
}

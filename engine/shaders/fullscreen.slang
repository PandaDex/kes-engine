// Fullscreen triangle helper shared by sky and blit passes (no vertex buffer).
#pragma once

// Plain vertex index without base-vertex offset. Slang lowers SV_VertexID to
// VertexIndex - BaseVertex on SPIR-V, which needs the DrawParameters capability.
#if defined(KES_TARGET_SPIRV)
#define KES_VERTEX_ID SV_VulkanVertexID
#else
#define KES_VERTEX_ID SV_VertexID
#endif

struct FullscreenVSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

FullscreenVSOut KesFullscreenTriangle(uint id)
{
    float2 pos[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    float2 uv[3] = {
        float2(0.0,  1.0),
        float2(0.0, -1.0),
        float2(2.0,  1.0)
    };

    FullscreenVSOut o;
    o.position = float4(pos[id], 0.0, 1.0);
    o.uv = uv[id];
    return o;
}

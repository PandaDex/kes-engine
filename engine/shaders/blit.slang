// Copies the low-res offscreen scene to the backbuffer.
#include "fullscreen.slang"

[[vk::binding(0, 0)]]
Sampler2D tex0 : register(t0);

[shader("vertex")]
FullscreenVSOut vsMain(uint id : KES_VERTEX_ID)
{
    return KesFullscreenTriangle(id);
}

[shader("fragment")]
float4 psMain(FullscreenVSOut input) : SV_Target
{
    return tex0.Sample(input.uv);
}

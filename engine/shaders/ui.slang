// 2D UI pass (RmlUi documents, ui_system.h), drawn on the backbuffer after the
// scene blit. Vertices are in backbuffer pixels (origin top-left), colors and
// textures are premultiplied alpha (blend: ONE, ONE_MINUS_SRC_ALPHA).

struct UiConstants
{
    // xy = scale, zw = offset: clip.xy = position * scale + offset.
    // The renderer bakes the backend's Y direction in, so the shader is the
    // same for Vulkan and D3D.
    float4 scaleOffset;
};

[[vk::push_constant]]
ConstantBuffer<UiConstants> uUi;   // b0 on D3D (only constant buffer)

// Set 0 binding 0 on Vulkan, t0/s0 on D3D. Untextured geometry samples a
// 1x1 white texture.
[[vk::binding(0, 0)]]
Sampler2D uUiTexture : register(t0);

struct UiVertexInput
{
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float4 color    : COLOR;
    [[vk::location(2)]] float2 uv       : TEXCOORD0;
};

struct UiVSOut
{
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
UiVSOut vsMain(UiVertexInput input)
{
    UiVSOut o;
    o.position = float4(input.position * uUi.scaleOffset.xy + uUi.scaleOffset.zw, 0.0, 1.0);
    o.color = input.color;
    o.uv = input.uv;
    return o;
}

[shader("fragment")]
float4 psMain(UiVSOut input) : SV_Target
{
    return input.color * uUiTexture.Sample(input.uv);
}

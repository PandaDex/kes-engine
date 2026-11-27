// SceneCB, shared by the mesh shaders (through kes_common.slang) and the
// fullscreen passes that need the camera (screen_space_lighting.slang).
// Layout: render::SceneCBData (engine/render_common.h).

#pragma once

static const int kMaxDirectionalLights = 4;
static const int kMaxCascades = 4;
static const int kMaxShadowTiles = 128;
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
    float4 uLightCounts;           // x = directional light count
    GPULight uLights[kMaxDirectionalLights];
    float4 uFogColor;
    float4 uFogParams;             // x = start, y = end, z = enabled, w = max density
    row_major float4x4 uCascadeViewProj[kMaxCascades];
    float4 uCascadeSplits;
    float4 uCascadeTexelSize;
    float4 uShadowParams;          // x = cascade count, y = 1 / cascade res, z = 1 / atlas size, w = enabled
    row_major float4x4 uLocalShadowViewProj[kMaxShadowTiles];
    float4 uLocalShadowRects[kMaxShadowTiles]; // atlas uv: xy = scale, zw = offset
    float4 uClusterParams;         // xyz = cluster grid size, w = cluster light count
    float4 uClusterDepth;          // x = near, y = slices / log(far / near)
    row_major float4x4 uInvViewProj;
    float4 uScreenParams;          // xy = render size, zw = 1 / render size
    float4 uDepthParams;           // x = near, y = far
    float4 uSsaoParams;            // x = radius (m), y = intensity, z = 1 AO on, w = 1 contact shadows on
    float4 uContactShadowParams;   // x = length (m), y = thickness (m), z = directional light index
    row_major float4x4 uPrevViewProj; // last frame's, without TAA jitter, for reprojecting into the history colors
    float4 uSsrParams;             // x = max distance (m), y = max roughness, z = 1 on, w = intensity
    float4 uDofParams;             // x = focus distance (m), y = focus range (m), z = max blur radius (px), w = 1 on
    float4 uTaaParams;             // xy = this frame's jitter in uv, z = 1 TAA on, w = 1 history valid
    float4 uMotionBlurParams;      // x = intensity (shutter fraction), y = max length (uv), z = 1 on
};

bool KesScreenReflectionsEnabled()
{
    return uSsrParams.z > 0.5;
}

// Screen-space lighting flags (uSsaoParams): what the main pass reads from uScreenLighting.
bool KesScreenAOEnabled()
{
    return uSsaoParams.z > 0.5;
}

bool KesContactShadowsEnabled()
{
    return uSsaoParams.w > 0.5;
}

// Render target uv of a world position (top-left origin in every backend).
float2 KesScreenUV(float3 worldPos)
{
    float4 clip = mul(float4(worldPos, 1.0), uViewProj);
    float2 ndc = clip.xy / max(clip.w, 1e-6);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Hardware depth (0-1, LH, not reversed) to view distance along the camera forward.
float KesLinearDepth(float depth)
{
    float n = uDepthParams.x;
    float f = uDepthParams.y;
    return n * f / (f - depth * (f - n));
}

float3 KesWorldPositionFromDepth(float2 uv, float depth)
{
    float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 world = mul(ndc, uInvViewProj);
    return world.xyz / world.w;
}

float KesViewDepth(float3 worldPos)
{
    return dot(worldPos - uCameraPosition.xyz, uCameraForward.xyz);
}



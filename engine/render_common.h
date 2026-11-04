#pragma once
// Backend-independent renderer code shared by Vulkan and D3D11: constant
// buffer layouts (must match engine/shaders/kes_common.slang), per-frame
// light / shadow / fog setup, the sky-based ambient environment (IBL), mip
// generation and frustum culling. Backends only upload what this produces.

#include <cstddef>
#include <cstdint>
#include <vector>
#include "camera.h"
#include "engine_types.h"
#include "material.h"
#include "render_backend.h"

class Scene;
struct SkySettings;

namespace render
{
    constexpr int kMaxLights = 32;
    constexpr int kMaxCascades = 4;
    constexpr int kMaxLocalShadowSlices = 16;
    constexpr int kEnvironmentFaceSize = 128;
    constexpr int kEnvironmentMipCount = 6;   // 128 .. 4; mip = roughness * (count - 1)
    constexpr int kMaterialTextureCount = 5;  // base, normal, metallicRoughness, occlusion, emissive

    // Row-major, multiplied as mul(vector, matrix) like the shaders.
    struct Matrix4
    {
        float m[16] = {};
    };
    Matrix4 MatrixIdentity();
    Matrix4 MatrixMultiply(const Matrix4& a, const Matrix4& b);
    Matrix4 MatrixLookToLH(const Vector3& eye, const Vector3& forward, const Vector3& up);
    Matrix4 MatrixOrthoOffCenterLH(float left, float right, float bottom, float top, float zn, float zf);
    Matrix4 MatrixPerspectiveFovLH(float fovY, float aspect, float zn, float zf);
    Vector3 TransformPoint(const Matrix4& m, const Vector3& p, float* outW = nullptr);

    // GPU layouts, std140 / HLSL cbuffer compatible (float4 granularity).
    struct GPULight
    {
        float position[4];   // xyz, w = range
        float color[4];      // rgb = radiance (color * intensity * pi), w unused
        float direction[4];  // xyz = direction the light travels, w = spot outer cos
        float params[4];     // x = type (0 dir, 1 point, 2 spot), y = spot inner cos,
                             // z = shadow: directional 1 = sun cascades, local first slice; -1 none
    };

    struct SceneCBData
    {
        float viewProj[16];
        float cameraPosition[4];       // xyz
        float cameraForward[4];        // xyz (view depth for cascade selection)
        float ambientSH[9][4];        // irradiance / pi, pre-convolved (rgb)
        float environmentParams[4];    // x = specular mip count, y = environment intensity, z = time in seconds
        float lightCounts[4];          // x = light count
        GPULight lights[kMaxLights];
        float fogColor[4];
        float fogParams[4];            // x = start, y = end, z = enabled, w = max density
        float cascadeViewProj[kMaxCascades][16];
        float cascadeSplits[4];        // view-space depth where each cascade ends
        float cascadeTexelSize[4];     // world size of one shadow texel per cascade (normal offset)
        float shadowParams[4];         // x = cascade count, y = 1 / cascade resolution, z = 1 / local resolution, w = enabled
        float localShadowViewProj[kMaxLocalShadowSlices][16];
    };

    struct MaterialCBData
    {
        float baseColor[4];
        float emissive[4];     // rgb = emissive color * intensity, a = alpha cutoff
        float pbr[4];          // x = metallic, y = roughness, z = normal scale, w = occlusion strength
        uint32_t flags[4];     // x = MaterialFlags incl. the Has*Texture bits
    };

    struct ModelCBData
    {
        float worldViewProj[16];
        float world[16];
        float skinInfo[4];
        float skinMatrices[128 * 16];   // last: static draws only need what comes before
    };

    struct SkyCBData
    {
        float topColor[4];
        float horizonColor[4];
        float bottomColor[4];
        float sunColor[4];
        float sunDirection[4];
        float cameraForward[4];
        float cameraRight[4];
        float cameraUp[4];
        float projectionParams[4];
        float skyParams[4];
    };

    struct TonemapCBData
    {
        float params[4];   // x = exposure, y = dither strength
    };

    // Offsets as Slang lays out kes_common.slang (checked with -reflection-json).
    static_assert(offsetof(SceneCBData, ambientSH) == 96, "SceneCB layout");
    static_assert(offsetof(SceneCBData, lights) == 272, "SceneCB layout");
    static_assert(offsetof(SceneCBData, fogColor) == 2320, "SceneCB layout");
    static_assert(offsetof(SceneCBData, cascadeViewProj) == 2352, "SceneCB layout");
    static_assert(offsetof(SceneCBData, shadowParams) == 2640, "SceneCB layout");
    static_assert(offsetof(SceneCBData, localShadowViewProj) == 2656, "SceneCB layout");
    static_assert(sizeof(SceneCBData) == 3680, "SceneCB layout");
    static_assert(offsetof(ModelCBData, skinInfo) == 128, "ModelCB layout");
    static_assert(offsetof(ModelCBData, skinMatrices) == 144, "ModelCB layout");
    static_assert(sizeof(MaterialCBData) == 64, "MaterialCB layout");

    MaterialCBData BuildMaterialConstants(const MaterialData* material, uint32_t textureFlags);
    SkyCBData BuildSkyConstants(const SkySettings& sky, const Camera& camera, float aspect);

    // Shadows
    struct ShadowConfig
    {
        int cascadeCount = 0;
        int cascadeResolution = 0;
        int localSlices = 0;
        int localResolution = 0;
    };
    ShadowConfig GetShadowConfig(ShadowQuality quality);

    struct ShadowView
    {
        Matrix4 viewProj;
        int slice = 0;          // layer in the cascade or the local shadow array
        bool cascade = false;
        Vector3 origin;         // light position (local) for distance culling
        float radius = 0.0f;    // local: light range; cascade: unused
    };

    struct Frustum
    {
        float planes[6][4] = {};   // ax + by + cz + d >= 0 inside
    };
    Frustum ExtractFrustum(const Matrix4& viewProj);
    bool SphereInFrustum(const Frustum& frustum, const Vector3& center, float radius);

    // Everything a backend needs to render one frame of the scene.
    struct FrameSetup
    {
        SceneCBData scene = {};
        Matrix4 viewProj;
        Frustum frustum;
        std::vector<ShadowView> shadowViews;
        ShadowConfig shadows;
    };
    void BuildFrameSetup(FrameSetup& out, const SceneState& state, const Camera& camera, const Scene& scene,
                         float aspect, const GraphicsOptions& options);

    // Ambient environment (IBL) from the sky gradient, or the flat
    // SceneState::ambient when no sky is active. Rebuilt only when its key changes.
    struct EnvironmentMaps
    {
        uint64_t key = 0;
        int faceSize = kEnvironmentFaceSize;
        int mipCount = kEnvironmentMipCount;
        // RGBA16F texels: faces[mip][face], face order +X -X +Y -Y +Z -Z, rows top to bottom.
        std::vector<uint16_t> faces[kEnvironmentMipCount][6];
        float sh[9][4] = {};
    };
    uint64_t EnvironmentKey(const SceneState& state, const Scene& scene);
    void BuildEnvironment(const SceneState& state, const Scene& scene, EnvironmentMaps& out);
    // Copies the SH coefficients into the scene constants.
    void ApplyEnvironment(const EnvironmentMaps& environment, SceneCBData& scene);

    // Mip chain for an RGBA8 image. sRGB images are filtered in linear space,
    // normal maps are renormalized. levels[0] is a copy of the source.
    struct MipLevel
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
    };
    enum class TextureUsage
    {
        Color,    // sRGB (base color, emissive)
        Linear,   // data (metallic/roughness, occlusion)
        Normal,
    };
    void GenerateMips(const uint8_t* rgba, uint32_t width, uint32_t height, TextureUsage usage, std::vector<MipLevel>& outLevels);

    // Local-space bounding sphere of a vertex stream (positions as 3 floats at `stride` bytes).
    void ComputeBoundingSphere(const void* vertices, size_t count, size_t stride, Vector3& outCenter, float& outRadius);
    // World-space sphere of a local sphere, `world` being TransformToMatrix of a transform with this scale.
    void TransformSphere(const Matrix4& world, const Vector3& scale, const Vector3& localCenter, float localRadius, Vector3& outCenter, float& outRadius);
    Matrix4 TransformToMatrix(const Transform& transform);

    uint16_t FloatToHalf(float value);
}

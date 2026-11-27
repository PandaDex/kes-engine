#pragma once
// Backend-independent renderer code shared by Vulkan and D3D11: constant
// buffer layouts (must match engine/shaders/kes_common.slang), per-frame
// light / shadow / fog setup, the sky-based ambient environment (IBL), mip
// generation and frustum culling. Backends only upload what this produces.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "camera.h"
#include "engine_types.h"
#include "material.h"
#include "mesh.h"
#include "render_backend.h"
#include "particle_sim.h"

class Scene;
struct SkySettings;
struct PostProcessSettings;
struct SceneObject;

namespace render
{
    constexpr int kMaxDirectionalLights = 4;
    constexpr int kMaxCascades = 4;
    // Local (spot/point) shadows: tiles in one depth atlas, a spot uses one,
    // a point light six (cube faces). Tile sizes are powers of two from
    // kMinShadowTile up to ShadowConfig::maxTile.
    constexpr int kMaxShadowTiles = 128; // per frame, SceneCB arrays
    constexpr int kMinShadowTile = 128;
    constexpr int kEnvironmentFaceSize = 128;
    constexpr int kEnvironmentMipCount = 6; // 128 .. 4; mip = roughness * (count - 1)
    constexpr int kMaterialTextureCount = 5; // base, normal, metallicRoughness, occlusion, emissive

    // Clustered local lights (point/spot lights of the scene, then particle
    // lights and Scene::SubmitPointLight): a froxel grid over the view, binned
    // on the CPU. The grid buffer starts with one header per cluster
    // ((first index << 8) | count), the light indices follow.
    constexpr int kClusterX = 16;
    constexpr int kClusterY = 9;
    constexpr int kClusterZ = 24;
    constexpr int kClusterCount = kClusterX * kClusterY * kClusterZ;
    constexpr int kMaxClusterLights = 4096;
    constexpr int kMaxClusterIndices = 256 * 1024;
    constexpr int kClusterGridUints = kClusterCount + kMaxClusterIndices;
    constexpr uint32_t kMaxLightsPerCluster = 255;

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
        float position[4]; // xyz, w = range
        float color[4]; // rgb = radiance (color * intensity * pi), w unused
        float direction[4]; // xyz = direction the light travels, w = spot outer cos
        float params[4]; // x = type (0 dir, 1 point, 2 spot), y = spot inner cos,
        // z = shadow: directional 1 = sun cascades, local first slice; -1 none
    };

    struct SceneCBData
    {
        float viewProj[16];
        float cameraPosition[4]; // xyz
        float cameraForward[4]; // xyz (view depth for cascade selection)
        float ambientSH[9][4]; // irradiance / pi, pre-convolved (rgb)
        float environmentParams[4]; // x = specular mip count, y = environment intensity, z = time in seconds
        float lightCounts[4]; // x = directional light count
        GPULight lights[kMaxDirectionalLights];
        float fogColor[4];
        float fogParams[4]; // x = start, y = end, z = enabled, w = max density
        float cascadeViewProj[kMaxCascades][16];
        float cascadeSplits[4]; // view-space depth where each cascade ends
        float cascadeTexelSize[4]; // world size of one shadow texel per cascade (normal offset)
        float shadowParams[4]; // x = cascade count, y = 1 / cascade resolution, z = 1 / atlas size, w = enabled
        float localShadowViewProj[kMaxShadowTiles][16];
        float localShadowRects[kMaxShadowTiles][4]; // atlas uv: xy = scale, zw = offset
        float clusterParams[4]; // xyz = grid size, w = cluster light count
        float clusterDepth[4]; // x = near, y = slices / log(far / near)
        float invViewProj[16];
        float screenParams[4]; // xy = render size, zw = 1 / render size
        float depthParams[4]; // x = near, y = far
        float ssaoParams[4]; // x = radius, y = intensity, z = 1 AO on, w = 1 contact shadows on
        float contactShadowParams[4]; // x = length, y = thickness, z = directional light index (-1 none)
        float prevViewProj[16];
        float ssrParams[4]; // x = max distance, y = max roughness, z = 1 on, w = intensity
        float dofParams[4]; // x = focus distance, y = focus range, z = max blur radius in pixels, w = 1 on
        float taaParams[4]; // xy = jitter in uv, z = 1 TAA on, w = 1 history valid
        float motionBlurParams[4]; // x = intensity, y = max length in uv, z = 1 on
    };

    struct MaterialCBData
    {
        float baseColor[4];
        float emissive[4]; // rgb = emissive color * intensity, a = alpha cutoff
        float pbr[4]; // x = metallic, y = roughness, z = normal scale, w = occlusion strength
        uint32_t flags[4]; // x = MaterialFlags incl. the Has*Texture bits
    };

    struct ModelCBData
    {
        float worldViewProj[16];
        float world[16];
        float skinInfo[4];
        float skinMatrices[128 * 16]; // last: static draws only need what comes before
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

    // present.slang and the bloom passes.
    struct BlitCBData
    {
        float params[4];
    };

    // blit.slang (tonemap).
    struct TonemapCBData
    {
        float params[4]; // x = exposure multiplier, y = dither strength, z = 1 multiply by the adapted exposure, w = bloom intensity
        float vignette[4]; // x = intensity, y = smoothness, z = aspect (width / height), w = 1 / bloom level count
        float vignetteColor[4]; // rgb, w = tonemapper
        float whiteBalance[4]; // xyz = LMS scale, w = saturation
        float colorFilter[4]; // rgb, w = contrast
        float lift[4]; // rgb, w = sharpen strength
        float gamma[4]; // rgb = 1 / gamma, w = 1 / render width
        float gain[4]; // rgb, w = 1 / render height
    };

    // exposure_histogram.slang / exposure_adapt.slang.
    struct ExposureCBData
    {
        float params0[4]; // x = histogram min log2 luminance, y = 1 / histogram log2 range, z = width, w = height
        float params1[4]; // x = delta time, y = speed up, z = speed down, w = 1 snap to the target
        float params2[4]; // x = adaptation min log2 luminance, y = max
    };

    constexpr int kBloomMaxLevels = 6; // level 0 is half the render resolution
    constexpr float kHistogramMinLog2 = -12.0f;
    constexpr float kHistogramLog2Range = 18.0f;
    constexpr uint32_t kHistogramBins = 256;
    constexpr uint32_t kExposureStateFloats = 4; // x = exposure, y = adapted log2 luminance, z = 1 once initialized

    // Bloom chain sizes for a render resolution; returns the level count (0 when too small).
    int ComputeBloomLevels(int renderWidth, int renderHeight, int outSizes[kBloomMaxLevels][2]);
    struct FrameSetup;

    // What the backend can run this frame for the screen-space passes.
    struct ScreenSpaceAvailability
    {
        bool lighting = false; // prepass + AO / contact shadow targets
        bool reflections = false; // reflection target and a history color from last frame
        bool depthOfField = false;
        bool temporalAA = false;
        bool temporalHistory = false; // TAA history from last frame
        bool motionBlur = false;
        const float* prevViewProj = nullptr; // last frame's camera matrix without jitter
    };

    // Screen-space constants of the frame (AO, contact shadows, reflections,
    // depth of field, TAA, motion blur).
    void SetScreenSpaceLighting(SceneCBData& cb, const PostProcessSettings& post, const GraphicsOptions& options,
        int renderWidth, int renderHeight, const ScreenSpaceAvailability& available);
    // Sub-pixel projection offset for TAA (Halton 2,3; 8 phases). Changes
    // out.viewProj and the scene constants, not the culling frustum.
    void ApplyTemporalJitter(FrameSetup& out, uint32_t frameIndex, int renderWidth, int renderHeight);
    bool WantsDepthPrepass(const SceneCBData& cb);
    bool WantsScreenReflections(const PostProcessSettings& post, const GraphicsOptions& options);
    bool WantsDepthOfField(const PostProcessSettings& post, const GraphicsOptions& options);
    bool WantsMotionBlur(const PostProcessSettings& post, const GraphicsOptions& options);
    bool InvertMatrix(const float m[16], float out[16]);

    // Unity's white balance: LMS scale factors for a temperature / tint (-1..1).
    void ComputeWhiteBalanceLms(float temperature, float tint, float out[3]);
    TonemapCBData BuildTonemapConstants(const SceneState& state, const PostProcessSettings& post, bool autoExposure,
        int bloomLevels, int renderWidth, int renderHeight);
    ExposureCBData BuildExposureConstants(const PostProcessSettings& post, float deltaTime, bool reset, int renderWidth, int renderHeight);

    // Offsets as Slang lays out kes_common.slang (checked with -reflection-json).
    static_assert(offsetof(SceneCBData, ambientSH) == 96, "SceneCB layout");
    static_assert(offsetof(SceneCBData, lights) == 272, "SceneCB layout");
    static_assert(offsetof(SceneCBData, fogColor) == 528, "SceneCB layout");
    static_assert(offsetof(SceneCBData, cascadeViewProj) == 560, "SceneCB layout");
    static_assert(offsetof(SceneCBData, shadowParams) == 848, "SceneCB layout");
    static_assert(offsetof(SceneCBData, localShadowViewProj) == 864, "SceneCB layout");
    static_assert(offsetof(SceneCBData, localShadowRects) == 9056, "SceneCB layout");
    static_assert(offsetof(SceneCBData, clusterParams) == 11104, "SceneCB layout");
    static_assert(offsetof(SceneCBData, invViewProj) == 11136, "SceneCB layout");
    static_assert(offsetof(SceneCBData, prevViewProj) == 11264, "SceneCB layout");
    static_assert(sizeof(SceneCBData) == 11392, "SceneCB layout");
    static_assert(sizeof(SceneCBData) <= 16384, "SceneCB must fit Vulkan's minimum maxUniformBufferRange");
    static_assert(sizeof(GPULight) == 64, "GPULight layout");
    static_assert(offsetof(ModelCBData, skinInfo) == 128, "ModelCB layout");
    static_assert(offsetof(ModelCBData, skinMatrices) == 144, "ModelCB layout");
    static_assert(sizeof(MaterialCBData) == 64, "MaterialCB layout");
    static_assert(sizeof(TonemapCBData) == 128, "TonemapCB layout (Vulkan push constants: 128 bytes guaranteed)");
    static_assert(sizeof(ExposureCBData) == 48, "ExposureCB layout");

    MaterialCBData BuildMaterialConstants(const MaterialData* material, uint32_t textureFlags);
    SkyCBData BuildSkyConstants(const SkySettings& sky, const Camera& camera, float aspect);

    // Shadows
    struct ShadowConfig
    {
        int cascadeCount = 0;
        int cascadeResolution = 0;
        int atlasSize = 0; // local shadow atlas, square
        int maxTile = 0;
    };
    ShadowConfig GetShadowConfig(ShadowQuality quality);

    struct ShadowView
    {
        Matrix4 viewProj;
        int slice = 0; // cascade layer
        bool cascade = false;
        int x = 0, y = 0, size = 0; // local: tile in the atlas, pixels from the top left
        Vector3 origin; // light position (local) for distance culling
        float radius = 0.0f; // local: light range; cascade: unused
    };

    // A shadow-casting object as the renderer sees it this frame. `state`
    // changes whenever what it draws into a shadow map changes.
    struct ShadowCaster
    {
        static constexpr uint64_t kTransient = 1ull << 63; // no id (particle meshes): changed every frame
        uint64_t key = 0;
        uint64_t state = 0;
        Vector3 center;
        float radius = 0.0f;
    };

    // Local shadow tiles kept across frames: a light's tiles are only drawn
    // again when the light changes or a caster in its range moves.
    struct ShadowAtlasCache
    {
        struct Entry
        {
            uint64_t lightKey = 0; // Light::id, or its address when it has none
            uint64_t lightState = 0;
            int tileSize = 0;
            int tileCount = 0; // 1 spot, 6 point
            int x[6] = {}, y[6] = {}; // pixels
            Matrix4 viewProj[6];
            Vector3 origin; // light position and range, for caster culling
            float range = 0.0f;
            Vector3 center; // what the light reaches
            float radius = 0.0f;
            bool contentValid = false; // drawn with the current matrices and casters
            bool stale = false; // a caster changed; redraw when the budget allows
            uint64_t staleSince = 0;
            uint64_t lastUsedFrame = 0;
        };
        struct CasterRecord
        {
            uint64_t state = 0;
            Vector3 center;
            float radius = 0.0f;
            uint64_t seenFrame = 0;
        };
        int atlasSize = 0;
        int maxTile = 0;
        uint64_t frame = 0;
        std::vector<uint8_t> cells; // kMinShadowTile cells, 1 = used
        std::vector<Entry> entries;
        std::vector<int> frameEntries; // entries in use this frame, in CB tile order
        std::unordered_map<uint64_t, CasterRecord> casters;
    };
    // Drops every tile (atlas recreated, assets reloaded).
    void InvalidateShadowCache(ShadowAtlasCache& cache);

    struct Frustum
    {
        float planes[6][4] = {}; // ax + by + cz + d >= 0 inside
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
        std::vector<GPULight> clusterLights; // at most kMaxClusterLights
        std::vector<uint32_t> clusterGrid; // headers + used indices, at most kClusterGridUints
        ShadowAtlasCache shadowCache; // kept across frames
    };
    void BuildFrameSetup(FrameSetup& out, const SceneState& state, const Camera& camera, const Scene& scene,
        float aspect, const GraphicsOptions& options);
    // After the draw lists: compares the casters with the previous frame and
    // appends the local shadow tiles to draw this frame to out.shadowViews.
    // The backend must draw every view it appends.
    void UpdateLocalShadows(FrameSetup& out, const std::vector<ShadowCaster>& casters);

    // Casters from a backend's opaque draw items (one per object). Skinned
    // objects have no bounds here, so they get a generous sphere at their origin.
    template <typename DrawItemT, typename CastsShadowsFn>
    void GatherShadowCasters(const std::vector<DrawItemT>& items, CastsShadowsFn castsShadows, std::vector<ShadowCaster>& out)
    {
        out.clear();
        const void* previous = nullptr; // sections of one object (repeats elsewhere are harmless)
        for (const DrawItemT& item : items)
        {
            if (item.object == previous || !castsShadows(item.material))
                continue;
            previous = item.object;
            ShadowCaster caster;
            caster.key = item.object->id ? static_cast<uint64_t>(item.object->id) : reinterpret_cast<uintptr_t>(item.object) | ShadowCaster::kTransient;
            uint64_t h = 1469598103934665603ull;
            const auto mix = [&h](const void* data, size_t bytes)
            {
                const unsigned char* p = static_cast<const unsigned char*>(data);
                for (size_t i = 0; i < bytes; ++i)
                    h = (h ^ p[i]) * 1099511628211ull;
            };
            mix(item.world.m, sizeof(item.world.m));
            mix(&item.object->mesh, sizeof(item.object->mesh));
            mix(&item.object->model, sizeof(item.object->model));
            mix(&item.lod, sizeof(item.lod));
            if (item.skinned && !item.object->skinMatrices.empty())
                mix(item.object->skinMatrices.data(), item.object->skinMatrices.size() * sizeof(float));
            caster.state = h;
            if (item.boundsRadius > 0.0f)
            {
                caster.center = item.boundsCenter;
                caster.radius = item.boundsRadius;
            }
            else
            {
                caster.center = Vector3(item.world.m[12], item.world.m[13], item.world.m[14]);
                caster.radius = 4.0f;
            }
            out.push_back(caster);
        }
    }

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
        Color, // sRGB (base color, emissive)
        Linear, // data (metallic/roughness, occlusion)
        Normal,
    };
    void GenerateMips(const uint8_t* rgba, uint32_t width, uint32_t height, TextureUsage usage, std::vector<MipLevel>& outLevels);

    // Local-space bounding sphere of a vertex stream (positions as 3 floats at `stride` bytes).
    void ComputeBoundingSphere(const void* vertices, size_t count, size_t stride, Vector3& outCenter, float& outRadius);
    // World-space sphere of a local sphere, `world` being TransformToMatrix of a transform with this scale.
    void TransformSphere(const Matrix4& world, const Vector3& scale, const Vector3& localCenter, float localRadius, Vector3& outCenter, float& outRadius);
    Matrix4 TransformToMatrix(const Transform& transform);
    // outScale is what the bounding sphere has to be scaled by
    Matrix4 SceneObjectMatrix(const SceneObject& object, Vector3& outScale);
    // The object's material for a mesh section's slot (-1 or unassigned: the object's own material).
    MaterialHandle SectionMaterial(const SceneObject& object, int32_t materialSlot);

    // How much of the screen height a world-space sphere's diameter covers (1 = all of it).
    float ProjectedScreenSize(const Vector3& center, float radius, const Vector3& cameraPosition, float fovY);
    // 0 = the base mesh, n = lods[n - 1]. SceneObject::forcedLod, then GraphicsOptions::forcedLod,
    // win over the screen size (scaled by GraphicsOptions::lodBias).
    uint32_t SelectLod(const std::vector<MeshLod>& lods, float screenSize, const SceneObject& object, const GraphicsOptions& options);
    inline const std::vector<MeshSection>& LodSections(const std::vector<MeshSection>& base, const std::vector<MeshLod>& lods, uint32_t lod)
    {
        return lod == 0 || lod > lods.size() ? base : lods[lod - 1].sections;
    }

    uint16_t FloatToHalf(float value);

    // Particle sprites and ribbons of a frame, expanded for the camera into one
    // vertex/index stream. Draws are sorted back to front by batch center.
    struct ParticleDraw
    {
        MaterialHandle material = 0;
        ParticleBlend blend = ParticleBlend::Additive;
        bool lit = false;
        bool ribbon = false;
        bool subImages = false; // sprite flipbook: no default disc per frame
        int sortOrder = 0;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        float distanceSq = 0.0f;
    };
    struct ParticleGeometry
    {
        std::vector<ParticleVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<ParticleDraw> draws;
    };
    void BuildParticleGeometry(const Scene& scene, const Camera& camera, ParticleGeometry& out);
    // ModelCB.uSkinInfo for particle.slang: x default shape, y lit, z premultiply, w additive.
    void ParticleDrawFlags(const ParticleDraw& draw, float outSkinInfo[4]);
}

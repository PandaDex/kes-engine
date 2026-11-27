#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "engine_types.h"
#include "entity_manager.h"
#include "mesh.h"
#include "kmodel.h"
#include "material.h"
#include "particle_sim.h"

enum class LightType : uint32_t
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct SceneObject
{
    static constexpr uint32_t MaxSkinMatrices = 128;

    uint32_t id = 0;
    Transform transform;
    MeshHandle mesh = 0;
    ModelHandle model = 0;
    MaterialHandle material = 0;
    // Per material slot of the mesh sections; a missing or 0 entry draws with `material`.
    std::vector<MaterialHandle> slotMaterials;
    uint32_t skinMatrixCount = 0;
    std::vector<float> skinMatrices; // skinMatrixCount * 16, empty for static objects
    // Used instead of transform when set. transform.position is still used for sorting, so keep it in sync.
    bool useWorldMatrix = false;
    float worldMatrix[16] = {};
    bool active = true;
};

struct Light
{
    uint32_t id = 0;
    LightType type = LightType::Point;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f; // full cone angle, degrees
    Transform transform;
    bool active = true;
    // The first shadowed directional light gets the sun cascades; spot and
    // point lights get tiles in the local shadow atlas (nearest to the camera first).
    bool castShadows = false;
};

struct SkySettings
{
    float topColor[3] = { 0.09f, 0.16f, 0.29f };
    float horizonColor[3] = { 0.52f, 0.62f, 0.78f };
    float bottomColor[3] = { 0.03f, 0.03f, 0.05f };
    float sunColor[3] = { 1.0f, 0.94f, 0.82f };
    float sunDirection[3] = { -0.25f, -0.65f, 0.72f };
    float sunSize = 2.5f;
    float sunIntensity = 1.15f;
    float horizonExponent = 1.6f;
    float ambientIntensity = 1.0f; // scales the sky's ambient light and reflections
    bool active = false;
    uint64_t ownerEntityId = 0;
};

struct FogSettings
{
    float color[3] = { 0.45f, 0.49f, 0.55f };
    float nearDistance = 6.0f;
    float farDistance = 18.0f;
    float stepCount = 10.0f;
    bool active = false;
    uint64_t ownerEntityId = 0;
    int32_t priority = 0;
    float selectionVolume = 0.0f;
};

// Camera post processing: exposure (manual or eye adaptation), bloom, vignette.
// Exposure values are in EV (stops, powers of two).
struct PostProcessSettings
{
    float exposureCompensation = 0.0f; // EV added on top of manual or auto exposure

    bool autoExposure = true;
    // Range of the scene's average log2 luminance the eye adapts to. A darker
    // scene than autoExposureMin stays dark; brighter than the max stays bright.
    float autoExposureMin = -5.0f;
    float autoExposureMax = 3.0f;
    float adaptationSpeedUp = 3.0f; // towards a brighter scene, 1/s
    float adaptationSpeedDown = 1.0f; // towards a darker scene, 1/s

    float bloomIntensity = 0.05f; // 0 = off; share of the blurred image in the result
    float bloomRadius = 1.0f; // upsample filter scale, ~0.5-2

    float vignetteIntensity = 0.0f; // 0 = off, 1 = strong
    float vignetteSmoothness = 0.5f; // 0-1, falloff softness
    float vignetteColor[3] = { 0.0f, 0.0f, 0.0f };
};

// Which PostProcessSettings fields a layer sets. Fields it doesn't set keep
// the value from the layers below.
enum PostProcessOverride : uint32_t
{
    PostProcessOverride_ExposureCompensation = 1u << 0,
    PostProcessOverride_AutoExposure = 1u << 1,
    PostProcessOverride_AutoExposureMin = 1u << 2,
    PostProcessOverride_AutoExposureMax = 1u << 3,
    PostProcessOverride_AdaptationSpeedUp = 1u << 4,
    PostProcessOverride_AdaptationSpeedDown = 1u << 5,
    PostProcessOverride_BloomIntensity = 1u << 6,
    PostProcessOverride_BloomRadius = 1u << 7,
    PostProcessOverride_VignetteIntensity = 1u << 8,
    PostProcessOverride_VignetteSmoothness = 1u << 9,
    PostProcessOverride_VignetteColor = 1u << 10,
    PostProcessOverride_All = (1u << 11) - 1u,
};

// One contribution to the final post-process settings. Layers are applied in
// ascending priority over the scene defaults; each blends its overridden
// fields in by weight (0-1). Booleans switch once the weight passes 0.5.
struct PostProcessLayer
{
    PostProcessSettings settings;
    uint32_t overrides = PostProcessOverride_All;
    int32_t priority = 0;
    float weight = 1.0f;
};

using PostProcessLayerId = uint32_t;

// Shadowless point light for the clustered light list (particle lights,
// muzzle flashes, sparks). Hundreds can be on screen; they don't use the 32
// regular light slots. color is linear radiance before the pi scale regular
// lights get, like Light::color * Light::intensity.
using ScenePointLight = ParticleLightInstance;

struct SceneParticleSprites
{
    ParticleSpriteSettings settings;
    MaterialHandle material = 0;
    std::vector<ParticleSpriteInstance> sprites;
    Vector3 center; // for sorting against other particle batches
};

struct SceneParticleRibbon
{
    ParticleRibbonSettings settings;
    MaterialHandle material = 0;
    std::vector<ParticleRibbonPoint> points;
    Vector3 center;
};

// Camera-independent particle render data, rebuilt every frame by
// ParticleWorld; renderers expand it into quads and strips.
struct SceneParticles
{
    std::vector<SceneParticleSprites> sprites;
    std::vector<SceneParticleRibbon> ribbons;
    std::vector<SceneObject> meshObjects; // mesh particles, drawn like scene objects
    std::vector<ScenePointLight> lights;

    void Clear()
    {
        sprites.clear();
        ribbons.clear();
        meshObjects.clear();
        lights.clear();
    }
};

// Blends `layers` (any order) over `base`.
PostProcessSettings ResolvePostProcessLayers(const PostProcessSettings& base, const PostProcessLayer* const* layers, size_t count);

class Scene
{
public:
    void SetEntityManager(EntityManager* manager) { entityManager = manager; }

    uint32_t CreateObject(const SceneObject& object);
    bool RemoveObject(uint32_t id);
    SceneObject* FindObject(uint32_t id);
    const SceneObject* FindObject(uint32_t id) const;

    uint32_t CreateLight(const Light& light);
    bool RemoveLight(uint32_t id);
    Light* FindLight(uint32_t id);
    const Light* FindLight(uint32_t id) const;

    // Order isn't stable: removing an object moves the last one into its place.
    std::vector<SceneObject>& GetObjects() { return objects; }
    const std::vector<SceneObject>& GetObjects() const { return objects; }

    std::vector<Light>& GetLights() { return lights; }
    const std::vector<Light>& GetLights() const { return lights; }

    SkySettings& GetSky() { return sky; }
    const SkySettings& GetSky() const { return sky; }

    FogSettings& GetFog() { return fog; }
    const FogSettings& GetFog() const { return fog; }

    // Post processing. Game code changes the defaults or adds layers (kept
    // until removed, also across map loads); volumes submit a layer every
    // frame. The renderer uses GetPostProcess(), resolved once per frame.
    PostProcessSettings& GetPostProcessDefaults() { return postProcessDefaults; }
    const PostProcessSettings& GetPostProcessDefaults() const { return postProcessDefaults; }
    PostProcessLayerId AddPostProcessLayer(const PostProcessLayer& layer);
    PostProcessLayer* FindPostProcessLayer(PostProcessLayerId id);
    bool RemovePostProcessLayer(PostProcessLayerId id);
    void SubmitPostProcessLayer(const PostProcessLayer& layer); // this frame only
    void ClearSubmittedPostProcessLayers();
    void ResolvePostProcess();
    const PostProcessSettings& GetPostProcess() const { return postProcess; }
    // Snap eye adaptation to the current scene next frame (after a teleport, cut, map load).
    void ResetExposureAdaptation() { ++exposureResetSerial; }
    uint32_t GetExposureResetSerial() const { return exposureResetSerial; } // renderers snap when it changes

    // Particles, rebuilt by ParticleWorld every frame.
    SceneParticles& GetParticles() { return particles; }
    const SceneParticles& GetParticles() const { return particles; }

    // Shadowless point lights for this frame only (cleared at the start of
    // every World::Update). Use for short flashes; Light/CreateLight for lasting lights.
    void SubmitPointLight(const ScenePointLight& light) { submittedPointLights.push_back(light); }
    void ClearSubmittedPointLights() { submittedPointLights.clear(); }
    const std::vector<ScenePointLight>& GetSubmittedPointLights() const { return submittedPointLights; }

    // Keeps the post-process defaults and the code layers.
    void Clear();

    template<typename T>
    T* GetEntityOfClass()
    {
        return entityManager ? entityManager->GetEntityOfClass<T>() : nullptr;
    }

    template<typename T>
    const T* GetEntityOfClass() const
    {
        return entityManager ? entityManager->GetEntityOfClass<T>() : nullptr;
    }

    template<typename T>
    T* GetEntityOfClassWithTag(const std::string& tag)
    {
        return entityManager ? entityManager->GetEntityOfClassWithTag<T>(tag) : nullptr;
    }

    template<typename T>
    const T* GetEntityOfClassWithTag(const std::string& tag) const
    {
        return entityManager ? entityManager->GetEntityOfClassWithTag<T>(tag) : nullptr;
    }

private:
    std::vector<SceneObject> objects;
    std::unordered_map<uint32_t, size_t> objectIndices; // id -> index in objects
    std::vector<Light> lights;
    SkySettings sky;
    FogSettings fog;
    PostProcessSettings postProcessDefaults;
    PostProcessSettings postProcess; // resolved
    std::vector<std::pair<PostProcessLayerId, PostProcessLayer>> postProcessLayers;
    std::vector<PostProcessLayer> submittedPostProcessLayers;
    SceneParticles particles;
    std::vector<ScenePointLight> submittedPointLights;
    PostProcessLayerId nextPostProcessLayerId = 1;
    uint32_t exposureResetSerial = 0;
    EntityManager* entityManager = nullptr;
    uint32_t nextObjectId = 1;
    uint32_t nextLightId = 1;
};

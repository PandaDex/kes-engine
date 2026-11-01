#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "engine_types.h"
#include "entity_manager.h"
#include "mesh.h"
#include "kmodel.h"
#include "material.h"

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
    uint32_t skinMatrixCount = 0;
    float skinMatrices[MaxSkinMatrices * 16] = {};
    bool active = true;
};

struct Light
{
    uint32_t id = 0;
    LightType type = LightType::Point;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    float range = 10.0f;
    float spotAngle = 45.0f;
    Transform transform;
    bool active = true;
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

    std::vector<SceneObject>& GetObjects() { return objects; }
    const std::vector<SceneObject>& GetObjects() const { return objects; }

    std::vector<Light>& GetLights() { return lights; }
    const std::vector<Light>& GetLights() const { return lights; }

    SkySettings& GetSky() { return sky; }
    const SkySettings& GetSky() const { return sky; }

    FogSettings& GetFog() { return fog; }
    const FogSettings& GetFog() const { return fog; }

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
    std::vector<Light> lights;
    SkySettings sky;
    FogSettings fog;
    EntityManager* entityManager = nullptr;
    uint32_t nextObjectId = 1;
    uint32_t nextLightId = 1;
};

#pragma once
#include <string>
#include "engine_types.h"
#include "entity_manager.h"

class World;
class Scene;
class AssetSystem;
class AudioSystem;
class CollisionWorld;
struct InputState;
struct Camera;
class Entity;
class CameraEntity;
class MapScriptSystem;

struct GameContext
{
    World* world = nullptr;
    Scene* scene = nullptr;
    AssetSystem* assets = nullptr;
    AudioSystem* audio = nullptr;
    CollisionWorld* collision = nullptr;
    // How far the frame is between the last two fixed steps (0..1), for
    // drawing physics bodies smoothly at any frame rate.
    float fixedStepAlpha = 1.0f;
    EntityManager* entities = nullptr;
    MapScriptSystem* mapScript = nullptr;
    InputState* input = nullptr;
    Camera* mainCamera = nullptr;
    bool animationDebugEnabled = false;
    uint64_t activeCameraEntityId = 0;

    void Play2DSound(const char* soundFile, float volume = 1.0f, float pitch = 1.0f);
    void Play3DSound(const char* soundFile, const Vector3& location, float attenuation, float volume = 1.0f, float pitch = 1.0f);
    bool LoadMap(const char* mapName);
    bool SetActiveCameraEntity(uint64_t entityId);
    bool SetActiveCameraByTag(const std::string& tag);
    CameraEntity* GetActiveCameraEntity();
    const CameraEntity* GetActiveCameraEntity() const;
    bool IsCameraEntityActive(uint64_t entityId) const { return activeCameraEntityId != 0 && activeCameraEntityId == entityId; }
    bool ExecuteCommand(const std::string& command, Entity* self = nullptr, Entity* other = nullptr);

    template<typename T>
    T* GetEntityOfClass()
    {
        return entities ? entities->GetEntityOfClass<T>() : nullptr;
    }

    template<typename T>
    const T* GetEntityOfClass() const
    {
        return entities ? entities->GetEntityOfClass<T>() : nullptr;
    }

    template<typename T>
    T* GetEntityOfClassWithTag(const std::string& tag)
    {
        return entities ? entities->GetEntityOfClassWithTag<T>(tag) : nullptr;
    }

    template<typename T>
    const T* GetEntityOfClassWithTag(const std::string& tag) const
    {
        return entities ? entities->GetEntityOfClassWithTag<T>(tag) : nullptr;
    }
};

#pragma once
#include <string>
#include <utility>
#include <vector>
#include "scene.h"
#include "asset_system.h"
#include "collision_world.h"
#include "audio_system.h"
#include "game_context.h"
#include "entity_manager.h"
#include "entity_factory_registry.h"
#include "map_file.h"
#include "map_script.h"
#include "particle_world.h"

struct InputState;
class CameraEntity;

class World
{
public:
    bool Init(Camera* mainCamera = nullptr, InputState* input = nullptr);
    void Shutdown();
    void Update(float dt);
    void FixedUpdate(float fixedDt);
    bool LoadMap(const char* mapName);
    const std::string& GetCurrentMapName() const { return currentMapName; }
    void SetPaused(bool paused);
    bool IsPaused() const { return paused; }
    void QueueMapLoad(const std::string& mapName);
    void SetDeferMapLoads(bool defer) { deferMapLoads = defer; }
    bool HasPendingMapLoad() const { return !pendingMapLoad.empty(); }
    std::string TakePendingMapLoad();
    uint64_t GetLevelGeneration() const { return levelGeneration; }
    void RegisterEntityClass(const std::string& className, EntityFactoryFunction factory);
    void SetPlayerClassName(const std::string& className);
    const std::string& GetPlayerClassName() const;

    Scene& GetScene();
    const Scene& GetScene() const;

    AssetSystem& GetAssets();
    const AssetSystem& GetAssets() const;

    AudioSystem& GetAudio();

    GameContext& GetGameContext();
    const GameContext& GetGameContext() const;

    EntityManager& GetEntities();
    const EntityManager& GetEntities() const;

    MapScriptSystem& GetMapScript() { return mapScript; }
    ParticleWorld& GetParticles() { return particles; }

private:
    void ClearLevelState();
    void InstantiateMap(const CompiledMap& map);
    void LinkMapScript(const CompiledMap& map, const std::vector<Entity*>& spawned);
    void RegisterBuiltInEntityClasses();
    Entity* CreatePlayerEntity();
    CameraEntity* FindStartupCameraEntity() const;
    CameraEntity* EnsureDefaultPlayerCamera(const Vector3& position, const Vector3& rotation);
    void SyncContext();

private:
    Scene scene;
    AssetSystem assets;
    CollisionWorld collision;
    AudioSystem audio;
    GameContext gameContext;
    EntityManager entities;
    MapScriptSystem mapScript;
    ParticleWorld particles;
    EntityFactoryRegistry entityFactories;
    std::vector<std::pair<std::string, EntityFactoryFunction>> customEntityFactories;
    std::string pendingMapLoad;
    std::string currentMapName;
    std::string playerClassName;
    bool paused = false;
    bool deferMapLoads = false;
    uint64_t levelGeneration = 0;
};

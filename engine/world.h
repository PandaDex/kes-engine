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
    void QueueMapLoad(const std::string& mapName);
    void RegisterEntityClass(const std::string& className, EntityFactoryFunction factory);
    void SetPlayerClassName(const std::string& className);
    const std::string& GetPlayerClassName() const;

    Scene& GetScene();
    const Scene& GetScene() const;

    AssetSystem& GetAssets();
    const AssetSystem& GetAssets() const;

    GameContext& GetGameContext();
    const GameContext& GetGameContext() const;

    EntityManager& GetEntities();
    const EntityManager& GetEntities() const;

private:
    void ClearLevelState();
    void InstantiateMap(const CompiledMap& map);
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
    EntityFactoryRegistry entityFactories;
    std::vector<std::pair<std::string, EntityFactoryFunction>> customEntityFactories;
    std::string pendingMapLoad;
    std::string playerClassName;
};

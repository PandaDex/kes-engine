#include "world.h"
#include "crash_reporter.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <algorithm>
#include "ent_audio_source.h"
#include "ent_brush.h"
#include "ent_camera.h"
#include "ent_skinned_model.h"
#include "camera.h"
#include "engine_configuration.h"
#include "input.h"
#include "ent_light.h"
#include "ent_logic.h"
#include "ent_player.h"
#include "ent_player_start.h"
#include "ent_fog_volume.h"
#include "ent_skybox.h"
#include "ent_trigger.h"
#include "logger.h"
#include "text_parse_utils.h"
#include "profiler.h"

namespace
{
    constexpr const char* kBuiltInPlayerClassName = ENGINE_DEFAULT_PLAYER_CLASS_NAME;

    template <typename T>
    static void RegisterBuiltInEntityClass(EntityFactoryRegistry& registry, const char* className)
    {
        static_assert(std::is_base_of_v<Entity, T>, "Built-in entities must derive from Entity");
        registry.Register(className,
                          [](EntityManager& manager, GameContext& context) -> Entity*
                          {
                              (void)context;
                              return manager.CreateEntity<T>();
                          });
    }

    std::string FormatVector(const Vector3& v)
    {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%g %g %g", v.x, v.y, v.z);
        return buffer;
    }

    // Editor geometry -> engine mesh. The axis swap mirrors the geometry, which
    // also flips triangle winding, so every triangle is re-wound here.
    MeshData ConvertMesh(const MapMesh& source)
    {
        MeshData mesh;
        mesh.vertices.reserve(source.vertices.size());
        for (const MapVertex& v : source.vertices)
        {
            const Vector3 p = ConvertMapPosition(v.position);
            const Vector3 n = Vector3Normalize(ConvertMapDirection(v.normal));
            mesh.vertices.push_back({ p.x, p.y, p.z, n.x, n.y, n.z, v.u, v.v, 1.0f, 1.0f, 1.0f, 1.0f });
        }
        mesh.indices.reserve(source.indices.size());
        for (size_t i = 0; i + 2 < source.indices.size(); i += 3)
        {
            mesh.indices.push_back(source.indices[i]);
            mesh.indices.push_back(source.indices[i + 2]);
            mesh.indices.push_back(source.indices[i + 1]);
        }
        return mesh;
    }

    MapEntityGeometry::Hull ConvertHull(const MapBrush& brush)
    {
        MapEntityGeometry::Hull hull;
        const float scale = GetMapUnitScale();
        hull.planes.reserve(brush.planes.size());
        for (const MapPlane& plane : brush.planes)
            hull.planes.push_back({ ConvertMapDirection(plane.normal), plane.dist * scale });
        const Vector3 a = ConvertMapPosition(brush.mins);
        const Vector3 b = ConvertMapPosition(brush.maxs);
        hull.boundsMin = { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
        hull.boundsMax = { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
        return hull;
    }
}

bool World::Init(Camera* mainCamera, InputState* input)
{
    KES_ZONE_FN(WORLD);
    EngineLog("World initialization begin.");
    Shutdown();
    gameContext.mainCamera = mainCamera;
    gameContext.input = input;

    if (playerClassName.empty())
        playerClassName = kBuiltInPlayerClassName;

    if (!audio.Init())
    {
        EngineLog("World initialization failed while starting audio.");
        return false;
    }

    entityFactories.Clear();

    SyncContext();
    entities.SetGameContext(&gameContext);
    RegisterBuiltInEntityClasses();
    for (const auto& [className, factory] : customEntityFactories)
        entityFactories.Register(className, factory);
    EngineLog("World initialization complete.");
    return true;
}

void World::Shutdown()
{
    KES_ZONE_FN(WORLD);
    EngineLog("World shutdown begin.");
    pendingMapLoad.clear();
    currentMapName.clear();
    paused = false;
    ClearLevelState();
    audio.Shutdown();
    assets.Clear();
    gameContext.mainCamera = nullptr;
    gameContext.input = nullptr;
    SyncContext();
    EngineLog("World shutdown complete.");
}

void World::Update(float dt)
{
    KES_ZONE_FN(WORLD);
    // While paused, per-frame scene state that entities set (fog) is kept
    // as it was, so the paused frame looks the same.
    if (!paused)
    {
        scene.GetFog() = {};
        entities.Update(dt);
        mapScript.Update(dt);
        entities.DestroyPending();
    }
    if (gameContext.mainCamera)
    {
        AudioListener listener = {};
        listener.position = CameraGetPosition(*gameContext.mainCamera);
        listener.forward = CameraGetForward(*gameContext.mainCamera);
        listener.up = CameraGetUp(*gameContext.mainCamera);
        audio.SetListener(listener);
    }
    audio.Update(dt);

    if (pendingMapLoad.empty())
        return;

    std::string mapName = std::move(pendingMapLoad);
    pendingMapLoad.clear();
    LoadMap(mapName.c_str());
}

void World::FixedUpdate(float fixedDt)
{
    KES_ZONE_FN(WORLD);
    if (paused)
        return;
    entities.FixedUpdate(fixedDt);
    collision.Step(fixedDt);
}

void World::SetPaused(bool pause)
{
    if (paused == pause)
        return;
    paused = pause;
    audio.SetPaused(pause);
    EngineLogf("World %s.", pause ? "paused" : "resumed");
}

bool World::LoadMap(const char* mapName)
{
    KES_ZONE_FN(WORLD);
    KES_ZONE_TEXTF(WORLD, "%s", mapName ? mapName : "<null>");
    if ((entities.IsUpdating() || mapScript.IsDispatching()) && mapName && mapName[0])
    {
        QueueMapLoad(mapName);
        return true;
    }
    EngineLogf("Loading map: %s", mapName ? mapName : "<null>");

    CompiledMap map;
    std::string error;
    if (!mapName || !mapName[0] || !LoadCompiledMap(mapName, map, &error))
    {
        // The file is read before anything is torn down, so a bad map name
        // (typo in a trigger, missing pak) leaves the current level playable.
        EngineLogf("LoadMap failed for %s: %s%s", mapName ? mapName : "<null>", error.c_str(),
                   currentMapName.empty() ? "" : " Keeping the current level.");
        return false;
    }

    ClearLevelState();
    assets.Clear();
    InstantiateMap(map);
    currentMapName = mapName;
    CrashReporterSetTag("map", mapName);

    if (!CreatePlayerEntity())
        EngineLogf("No player entity was created for %s (player class \"%s\").", mapName, playerClassName.c_str());
    entities.StartPending();
    EngineLogf("Map loaded successfully: %s (%zu meshes, %zu brushes, %zu entities, %zu materials).",
               mapName ? mapName : "<null>", map.meshes.size(), map.brushes.size(), map.entities.size(), map.materials.size());
    return true;
}

void World::QueueMapLoad(const std::string& mapName)
{
    pendingMapLoad = mapName;
    EngineLogf("Queued map load: %s", mapName.c_str());
}

void World::RegisterEntityClass(const std::string& className, EntityFactoryFunction factory)
{
    if (className.empty() || !factory)
        return;

    bool updated = false;
    for (auto& entry : customEntityFactories)
    {
        if (entry.first == className)
        {
            entry.second = factory;
            updated = true;
            break;
        }
    }

    if (!updated)
        customEntityFactories.emplace_back(className, factory);

    entityFactories.Register(className, std::move(factory));
}

void World::SetPlayerClassName(const std::string& className)
{
    if (className.empty())
        return;

    playerClassName = className;
}

const std::string& World::GetPlayerClassName() const
{
    return playerClassName;
}

Scene& World::GetScene()
{
    return scene;
}

const Scene& World::GetScene() const
{
    return scene;
}

AudioSystem& World::GetAudio()
{
    return audio;
}

AssetSystem& World::GetAssets()
{
    return assets;
}

const AssetSystem& World::GetAssets() const
{
    return assets;
}

GameContext& World::GetGameContext()
{
    return gameContext;
}

const GameContext& World::GetGameContext() const
{
    return gameContext;
}

EntityManager& World::GetEntities()
{
    return entities;
}

const EntityManager& World::GetEntities() const
{
    return entities;
}

void World::ClearLevelState()
{
    KES_ZONE_FN(WORLD);
    entities.DestroyAll();
    mapScript.Clear();
    collision.Clear();
    scene.Clear();
    gameContext.activeCameraEntityId = 0;   // the camera entity is gone
    SyncContext();
}

void World::InstantiateMap(const CompiledMap& map)
{
    KES_ZONE_FN(WORLD);
    // Materials. "" (faces without a material) and missing files use the default material.
    MaterialHandle defaultMaterial = 0;
    auto getDefaultMaterial = [&]()
    {
        if (defaultMaterial == 0)
            defaultMaterial = assets.CreateMaterial(MaterialData{});
        return defaultMaterial;
    };
    std::vector<MaterialHandle> materials(map.materials.size(), 0);
    for (size_t i = 0; i < map.materials.size(); ++i)
    {
        if (!map.materials[i].empty())
        {
            materials[i] = assets.LoadMaterial(map.materials[i].c_str());
            if (materials[i] == 0)
                EngineLogf("Map material %s could not be loaded; using the default material.", map.materials[i].c_str());
        }
        if (materials[i] == 0)
            materials[i] = getDefaultMaterial();
    }
    auto materialFor = [&](uint32_t index)
    {
        return index < materials.size() ? materials[index] : getDefaultMaterial();
    };

    // World geometry and collision.
    for (const MapMesh& mesh : map.meshes)
    {
        if (mesh.entity >= 0 || mesh.indices.empty())
            continue;
        SceneObject object = {};
        object.mesh = assets.CreateMesh(ConvertMesh(mesh));
        object.material = materialFor(mesh.material);
        object.transform.scale = { 1.0f, 1.0f, 1.0f };
        scene.CreateObject(object);
    }
    for (const MapBrush& brush : map.brushes)
    {
        if (brush.entity >= 0 || brush.planes.empty())
            continue;
        const MapEntityGeometry::Hull hull = ConvertHull(brush);
        collision.AddConvex({}, hull.planes, hull.boundsMin, hull.boundsMax);
    }

    // Entities, created by class name. Keyvalues go through Entity::ApplyProperty.
    std::vector<Entity*> spawned(map.entities.size(), nullptr);
    for (size_t index = 0; index < map.entities.size(); ++index)
    {
        const MapEntity& source = map.entities[index];
        Entity* entity = entityFactories.Create(source.className, entities, gameContext);
        if (!entity)
        {
            EngineLogf("Map entity class \"%s\" is not registered; skipped.", source.className.c_str());
            continue;
        }
        spawned[index] = entity;
        if (source.guid.IsValid() && !entities.SetEntityGuid(*entity, source.guid))
            EngineLogf("Map entity \"%s\" (%s) reuses GUID %s; it keeps a runtime GUID and links to it are lost.",
                       source.name.c_str(), source.className.c_str(), source.guid.ToString().c_str());

        entity->Rotation = ConvertMapAngles(source.angles);
        entity->Teleport(ConvertMapPosition(source.origin));
        if (!source.name.empty())
            entity->Tags.push_back(source.name);   // map names double as tags (GetEntityOfClassWithTag)

        if (source.solid)
            entity->ApplyProperty("Size", FormatVector(ConvertMapExtents(source.maxs - source.mins)));

        for (const auto& [key, value] : source.keyValues)
        {
            if (key == "Tags")
            {
                for (const std::string& tag : textparse::SplitList(value))
                    entity->Tags.push_back(tag);
                continue;
            }
            entity->ApplyProperty(key, value);
        }

        if (source.solid)
        {
            MapEntityGeometry geometry;
            const Vector3 a = ConvertMapPosition(source.mins);
            const Vector3 b = ConvertMapPosition(source.maxs);
            geometry.boundsMin = { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
            geometry.boundsMax = { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
            for (const MapMesh& mesh : map.meshes)
            {
                if (mesh.entity == static_cast<int32_t>(index) && !mesh.indices.empty())
                    geometry.meshes.push_back({ assets.CreateMesh(ConvertMesh(mesh)), materialFor(mesh.material) });
            }
            for (const MapBrush& brush : map.brushes)
            {
                if (brush.entity == static_cast<int32_t>(index) && !brush.planes.empty())
                    geometry.hulls.push_back(ConvertHull(brush));
            }
            entity->ApplyMapGeometry(geometry);
        }
    }

    LinkMapScript(map, spawned);
}

// Links are registered after every entity exists, so each can be checked against the ports
// of both ends. Mismatches are logged but kept: ports are only a declaration.
void World::LinkMapScript(const CompiledMap& map, const std::vector<Entity*>& spawned)
{
    for (size_t index = 0; index < map.entities.size(); ++index)
    {
        const MapEntity& source = map.entities[index];
        Entity* entity = spawned[index];
        if (!entity || source.outputs.empty())
            continue;

        const IMapScriptable* scriptable = dynamic_cast<const IMapScriptable*>(entity);
        if (!scriptable)
        {
            EngineLogf("MapScript: %s \"%s\" has %zu link(s) but does not implement IMapScriptable; they never fire.",
                       source.className.c_str(), source.name.c_str(), source.outputs.size());
            continue;
        }

        for (const MapConnection& connection : source.outputs)
        {
            if (!scriptable->GetMapScriptPorts().HasOutput(connection.output))
                EngineLogf("MapScript: %s \"%s\" has no output \"%s\".",
                           source.className.c_str(), source.name.c_str(), connection.output.c_str());

            Entity* target = entities.GetEntityByGuid(connection.target);
            const IMapScriptable* targetScript = dynamic_cast<const IMapScriptable*>(target);
            if (!target)
                EngineLogf("MapScript: %s \"%s\" output \"%s\" targets %s, which is not in the map.",
                           source.className.c_str(), source.name.c_str(), connection.output.c_str(),
                           connection.target.ToString().c_str());
            else if (!targetScript)
                EngineLogf("MapScript: the target of %s \"%s\" output \"%s\" takes no inputs.",
                           source.className.c_str(), source.name.c_str(), connection.output.c_str());
            else if (!targetScript->GetMapScriptPorts().HasInput(connection.input))
                EngineLogf("MapScript: the target of %s \"%s\" output \"%s\" has no input \"%s\".",
                           source.className.c_str(), source.name.c_str(), connection.output.c_str(), connection.input.c_str());

            MapScriptLink link;
            link.output = connection.output;
            link.target = connection.target;
            link.input = connection.input;
            link.parameter = connection.param;
            link.delay = connection.delay;
            link.once = connection.once;
            mapScript.AddLink(entity->GetGuid(), std::move(link));
        }
    }
    if (const size_t count = mapScript.GetLinkCount())
        EngineLogf("MapScript: %zu link(s) loaded.", count);
}

void World::RegisterBuiltInEntityClasses()
{
    EngineLog("Registering built-in entity classes.");
    RegisterBuiltInEntityClass<PlayerStartEntity>(entityFactories, "PlayerStartEntity");
    RegisterBuiltInEntityClass<TriggerEntity>(entityFactories, "TriggerEntity");
    RegisterBuiltInEntityClass<LightEntity>(entityFactories, "LightEntity");
    RegisterBuiltInEntityClass<SkyboxEntity>(entityFactories, "SkyboxEntity");
    RegisterBuiltInEntityClass<CameraEntity>(entityFactories, "CameraEntity");
    RegisterBuiltInEntityClass<FogVolumeEntity>(entityFactories, "FogVolumeEntity");
    RegisterBuiltInEntityClass<AudioSourceEntity>(entityFactories, "AudioSourceEntity");
    RegisterBuiltInEntityClass<SkinnedModelEntity>(entityFactories, "SkinnedModelEntity");
    // A static mesh is a model without animation; same entity, clearer name in the editor.
    RegisterBuiltInEntityClass<SkinnedModelEntity>(entityFactories, "StaticMeshEntity");
    RegisterBuiltInEntityClass<BrushEntity>(entityFactories, "BrushEntity");
    RegisterBuiltInEntityClass<LogicRelayEntity>(entityFactories, "LogicRelayEntity");
    RegisterBuiltInEntityClass<LogicAutoEntity>(entityFactories, "LogicAutoEntity");

    entityFactories.Register(kBuiltInPlayerClassName,
                             [](EntityManager& manager, GameContext& context) -> Entity*
                             {
                                 if (!context.input)
                                     return nullptr;
                                 return manager.CreateEntity<EnginePlayerEntity>(*context.input);
                             });
}

Entity* World::CreatePlayerEntity()
{
    KES_ZONE_FN(WORLD);
    if (!gameContext.input)
        return nullptr;

    Vector3 spawnPosition = {};
    Vector3 spawnRotation = {};

    PlayerStartEntity* playerStart = scene.GetEntityOfClass<PlayerStartEntity>();
    if (playerStart)
    {
        spawnPosition = playerStart->Position;
        spawnRotation = playerStart->Rotation;
    }
    else if (gameContext.mainCamera)
    {
        spawnPosition = CameraGetPosition(*gameContext.mainCamera);
        spawnRotation = { gameContext.mainCamera->pitch, gameContext.mainCamera->yaw, 0.0f };
    }

    if (CameraEntity* startupCamera = FindStartupCameraEntity())
        gameContext.SetActiveCameraEntity(startupCamera->GetEntityId());
    else
        EnsureDefaultPlayerCamera(spawnPosition, spawnRotation);

    Entity* player = entityFactories.Create(playerClassName, entities, gameContext);
    if (!player && playerClassName != kBuiltInPlayerClassName)
    {
        EngineLogf("Player class %s is not registered. Falling back to %s.",
                   playerClassName.c_str(),
                   kBuiltInPlayerClassName);
        player = entityFactories.Create(kBuiltInPlayerClassName, entities, gameContext);
    }
    if (!player)
        return nullptr;

    player->Tags.push_back(kPlayerTag);
    player->Rotation = spawnRotation;
    player->Teleport(spawnPosition);
    return player;
}

CameraEntity* World::FindStartupCameraEntity() const
{
    CameraEntity* startupCamera = nullptr;
    entities.ForEachActive([&](Entity& entity)
    {
        CameraEntity* camera = dynamic_cast<CameraEntity*>(&entity);
        if (camera && camera->WantsToBeActiveOnStart())
            startupCamera = camera;
    });
    return startupCamera;
}

CameraEntity* World::EnsureDefaultPlayerCamera(const Vector3& position, const Vector3& rotation)
{
    if (CameraEntity* activeCamera = gameContext.GetActiveCameraEntity())
        return activeCamera;

    CameraEntity* camera = entities.CreateEntity<CameraEntity>();
    camera->SetWorldPose(position, rotation);
    camera->SetPlayerControlled(true);
    camera->SetActive(true);
    return camera;
}

void World::SyncContext()
{
    gameContext.world = this;
    gameContext.scene = &scene;
    gameContext.assets = &assets;
    gameContext.audio = &audio;
    gameContext.collision = &collision;
    gameContext.entities = &entities;
    gameContext.mapScript = &mapScript;
    mapScript.SetGameContext(&gameContext);
    scene.SetEntityManager(&entities);
}

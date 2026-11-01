#include "world.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include "ent_audio_source.h"
#include "ent_camera.h"
#include "ent_skinned_model.h"
#include "camera.h"
#include "engine_configuration.h"
#include "input.h"
#include "ent_light.h"
#include "ent_player.h"
#include "ent_player_start.h"
#include "ent_fog_volume.h"
#include "ent_skybox.h"
#include "ent_trigger.h"
#include "logger.h"
#include "text_parse_utils.h"

namespace
{
    constexpr const char* kBuiltInPlayerClassName = ENGINE_DEFAULT_PLAYER_CLASS_NAME;

    struct float4x4
    {
        float m[4][4];
    };

    auto kStartupObjPath = "meshes/startup.obj";

    float4x4 Identity()
    {
        float4x4 result = {};
        result.m[0][0] = 1.0f;
        result.m[1][1] = 1.0f;
        result.m[2][2] = 1.0f;
        result.m[3][3] = 1.0f;
        return result;
    }

    float4x4 Multiply(const float4x4& a, const float4x4& b)
    {
        float4x4 result = {};
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                result.m[row][col] =
                    a.m[row][0] * b.m[0][col] +
                    a.m[row][1] * b.m[1][col] +
                    a.m[row][2] * b.m[2][col] +
                    a.m[row][3] * b.m[3][col];
            }
        }
        return result;
    }

    float4x4 RotationX(const float angle)
    {
        float4x4 result = Identity();
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        result.m[1][1] = c;
        result.m[1][2] = s;
        result.m[2][1] = -s;
        result.m[2][2] = c;
        return result;
    }

    float4x4 RotationY(const float angle)
    {
        float4x4 result = Identity();
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        result.m[0][0] = c;
        result.m[0][2] = -s;
        result.m[2][0] = s;
        result.m[2][2] = c;
        return result;
    }

    float4x4 RotationZ(const float angle)
    {
        float4x4 result = Identity();
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        result.m[0][0] = c;
        result.m[0][1] = s;
        result.m[1][0] = -s;
        result.m[1][1] = c;
        return result;
    }

    float4x4 Scale(const float x, const float y, const float z)
    {
        float4x4 result = Identity();
        result.m[0][0] = x;
        result.m[1][1] = y;
        result.m[2][2] = z;
        return result;
    }

    float4x4 Translation(const float x, const float y,const float z)
    {
        float4x4 result = Identity();
        result.m[3][0] = x;
        result.m[3][1] = y;
        result.m[3][2] = z;
        return result;
    }

    static float VectorLength(float x, float y, float z)
    {
        return std::sqrt(x * x + y * y + z * z);
    }

    static float4x4 TransformToMatrix(const Transform& transform)
    {
        float4x4 result = Scale(transform.scale.x, transform.scale.y, transform.scale.z);
        result = Multiply(result, RotationX(transform.rotation.x));
        result = Multiply(result, RotationY(transform.rotation.y));
        result = Multiply(result, RotationZ(transform.rotation.z));
        result = Multiply(result, Translation(transform.position.x, transform.position.y, transform.position.z));
        return result;
    }

    static Transform MatrixToTransform(const float4x4& matrix)
    {
        Transform result = {};
        result.position.x = matrix.m[3][0];
        result.position.y = matrix.m[3][1];
        result.position.z = matrix.m[3][2];

        const float sx = VectorLength(matrix.m[0][0], matrix.m[0][1], matrix.m[0][2]);
        const float sy = VectorLength(matrix.m[1][0], matrix.m[1][1], matrix.m[1][2]);
        const float sz = VectorLength(matrix.m[2][0], matrix.m[2][1], matrix.m[2][2]);

        result.scale.x = sx > 0.00001f ? sx : 1.0f;
        result.scale.y = sy > 0.00001f ? sy : 1.0f;
        result.scale.z = sz > 0.00001f ? sz : 1.0f;

        const float r00 = matrix.m[0][0] / result.scale.x;
        const float r01 = matrix.m[0][1] / result.scale.x;
        float r02 = matrix.m[0][2] / result.scale.x;
        const float r10 = matrix.m[1][0] / result.scale.y;
        const float r11 = matrix.m[1][1] / result.scale.y;
        const float r12 = matrix.m[1][2] / result.scale.y;
        const float r22 = matrix.m[2][2] / result.scale.z;

        if (r02 < -1.0f) r02 = -1.0f;
        if (r02 > 1.0f) r02 = 1.0f;

        result.rotation.y = std::asin(-r02);
        const float cy = std::cos(result.rotation.y);
        if (std::fabs(cy) > 0.0001f)
        {
            result.rotation.x = std::atan2(r12, r22);
            result.rotation.z = std::atan2(r01, r00);
        }
        else
        {
            result.rotation.x = std::atan2(-r10, r11);
            result.rotation.z = 0.0f;
        }

        return result;
    }

    static Transform ComposeTransforms(const Transform& parent, const Transform& local)
    {
        return MatrixToTransform(Multiply(TransformToMatrix(local), TransformToMatrix(parent)));
    }

    static bool EndsWithInsensitive(const std::string& value, const char* suffix)
    {
        if (!suffix)
            return false;

        const size_t suffixLen = std::strlen(suffix);
        if (value.size() < suffixLen)
            return false;

        for (size_t i = 0; i < suffixLen; ++i)
        {
            const char a = (char)std::tolower((unsigned char)value[value.size() - suffixLen + i]);
            const char b = (char)std::tolower((unsigned char)suffix[i]);
            if (a != b)
                return false;
        }

        return true;
    }

    static void ParseTransformProperties(const LevelBlock& block, Transform& outTransform)
    {
        outTransform = {};
        outTransform.scale = { 1.0f, 1.0f, 1.0f };

        const std::string* position = FindLevelProperty(block, "Location");
        if (!position)
            position = FindLevelProperty(block, "Position");
        if (position)
        {
            const Vector3 parsed = textparse::ParseVector3(*position, {});
            outTransform.position = parsed;
        }

        if (const std::string* rotation = FindLevelProperty(block, "Rotation"))
        {
            const Vector3 parsed = textparse::ParseVector3(*rotation, {});
            outTransform.rotation = parsed;
        }

        if (const std::string* scale = FindLevelProperty(block, "Scale"))
        {
            const Vector3 parsed = textparse::ParseVector3(*scale, {1.0f, 1.0f, 1.0f});
            outTransform.scale = parsed;
        }
    }

    static MaterialData ParseMaterial(const LevelBlock& block)
    {
        MaterialData material = {};
        const std::string* materialValue = FindLevelProperty(block, "Material");
        if (!materialValue)
            materialValue = FindLevelProperty(block, "MaterialColor");

        if (materialValue)
        {
            const std::vector<std::string> parts = textparse::SplitList(*materialValue);
            if (parts.size() >= 3)
            {
                material.baseColor[0] = std::strtof(parts[0].c_str(), nullptr);
                material.baseColor[1] = std::strtof(parts[1].c_str(), nullptr);
                material.baseColor[2] = std::strtof(parts[2].c_str(), nullptr);
                material.baseColor[3] = parts.size() > 3 ? std::strtof(parts[3].c_str(), nullptr) : 1.0f;
            }
        }

        if (const std::string* emissiveValue = FindLevelProperty(block, "MaterialEmissive"))
        {
            const Vector3 emissive = textparse::ParseVector3(*emissiveValue, {});
            material.emissiveColor[0] = emissive.x;
            material.emissiveColor[1] = emissive.y;
            material.emissiveColor[2] = emissive.z;
        }
        else if (const std::string* emissiveValue = FindLevelProperty(block, "MaterialEmission"))
        {
            const Vector3 emissive = textparse::ParseVector3(*emissiveValue, {});
            material.emissiveColor[0] = emissive.x;
            material.emissiveColor[1] = emissive.y;
            material.emissiveColor[2] = emissive.z;
        }

        if (const std::string* unlitValue = FindLevelProperty(block, "MaterialUnlit"))
        {
            if (textparse::ParseBool(*unlitValue, false))
                material.flags &= ~MaterialFlag_Lit;
        }

        if (const std::string* doubleSidedValue = FindLevelProperty(block, "MaterialDoubleSided"))
        {
            if (textparse::ParseBool(*doubleSidedValue, false))
                material.flags |= MaterialFlag_DoubleSided;
        }

        if (const std::string* alphaCutoffValue = FindLevelProperty(block, "MaterialAlphaCutoff"))
        {
            material.alphaCutoff = textparse::ParseFloat(*alphaCutoffValue, material.alphaCutoff);
            if (material.alphaCutoff < 0.0f) material.alphaCutoff = 0.0f;
            if (material.alphaCutoff > 1.0f) material.alphaCutoff = 1.0f;
        }

        if (const std::string* textureValue = FindLevelProperty(block, "MaterialTexture"))
            material.baseTexturePath = textparse::Unquote(*textureValue);
        else if (const std::string* textureValue = FindLevelProperty(block, "MaterialBaseTexture"))
            material.baseTexturePath = textparse::Unquote(*textureValue);

        bool transparent = material.baseColor[3] < 0.999f;
        if (const std::string* transparentValue = FindLevelProperty(block, "MaterialTransparent"))
            transparent = textparse::ParseBool(*transparentValue, transparent);

        if (transparent)
            material.flags |= MaterialFlag_Transparent;
        if (!material.baseTexturePath.empty())
            material.flags |= MaterialFlag_Textured;

        return material;
    }

    static std::vector<std::string> ParseTags(const std::string* tagsValue)
    {
        if (!tagsValue)
            return {};
        return textparse::SplitList(*tagsValue);
    }

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
}

bool World::Init(Camera* mainCamera, InputState* input)
{
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
    prefabs.Clear();

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
    EngineLog("World shutdown begin.");
    pendingMapLoad.clear();
    ClearLevelState();
    audio.Shutdown();
    assets.Clear();
    prefabs.Clear();
    mainObjectId = 0;
    gameContext.mainCamera = nullptr;
    gameContext.input = nullptr;
    SyncContext();
    EngineLog("World shutdown complete.");
}

void World::Update(float dt)
{
    scene.GetFog() = {};
    entities.Update(dt);
    entities.DestroyPending();
    audio.Update(dt);

    if (pendingMapLoad.empty())
        return;

    std::string mapName = std::move(pendingMapLoad);
    pendingMapLoad.clear();
    LoadMap(mapName.c_str());
}

void World::FixedUpdate(float fixedDt)
{
    entities.FixedUpdate(fixedDt);
}

bool World::LoadMap(const char* mapName)
{
    EngineLogf("Loading map: %s", mapName ? mapName : "<null>");
    const auto resetLoadedState = [this]()
    {
        ClearLevelState();
        assets.Clear();
    };

    std::vector<LevelBlock> blocks;
    std::string error;
    if (!LoadLevelFile(mapName, blocks, &error))
    {
        EngineLogf("LoadMap failed for %s: %s", mapName ? mapName : "<null>", error.c_str());
        resetLoadedState();
        return false;
    }

    resetLoadedState();

    Transform root = {};
    root.scale = { 1.0f, 1.0f, 1.0f };

    if (!InstantiateMap(blocks, root))
    {
        EngineLogf("LoadMap failed while instantiating %s", mapName ? mapName : "<null>");
        resetLoadedState();
        return false;
    }

    (void)CreatePlayerEntity();
    entities.StartPending();
    EngineLogf("Map loaded successfully: %s (%zu blocks).", mapName ? mapName : "<null>", blocks.size());
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
    entities.DestroyAll();
    collision.Clear();
    scene.Clear();
    mainObjectId = 0;
    SyncContext();
}

bool World::InstantiateMap(const std::vector<LevelBlock>& blocks, const Transform& rootTransform)
{
    for (const LevelBlock& block : blocks)
    {
        bool ok = false;

        switch (block.type)
        {
        case LevelBlockType::Object:
            ok = InstantiateObjectBlock(block, rootTransform);
            break;
        case LevelBlockType::Entity:
            ok = InstantiateEntityBlock(block, rootTransform);
            break;
        case LevelBlockType::Collider:
            ok = InstantiateColliderBlock(block, rootTransform);
            break;
        case LevelBlockType::Prefab:
            ok = InstantiatePrefabBlock(block, rootTransform);
            break;
        default:
            ok = false;
            break;
        }

        if (!ok)
        {
            EngineLogf("Failed to instantiate %s block declared near line %d", GetLevelBlockTypeName(block.type),
                       block.line);
            return false;
        }
    }

    return true;
}

bool World::InstantiateObjectBlock(const LevelBlock& block, const Transform& rootTransform)
{
    const std::string* meshValue = FindLevelProperty(block, "Mesh");
    if (!meshValue)
        return false;

    Transform local = {};
    ParseTransformProperties(block, local);
    const Transform transform = ComposeTransforms(rootTransform, local);

    MeshHandle mesh = assets.LoadMesh(textparse::Unquote(*meshValue).c_str());
    if (mesh == 0)
        mesh = assets.CreateCubeMesh();

    MaterialHandle materialHandle = 0;
    if (const std::string* materialFile = FindLevelProperty(block, "MaterialFile"))
    {
        materialHandle = assets.LoadMaterial(textparse::Unquote(*materialFile).c_str());
    }
    else if (const std::string* materialValue = FindLevelProperty(block, "Material"))
    {
        const std::string rawMaterial = textparse::Unquote(*materialValue);
        if (EndsWithInsensitive(rawMaterial, ".mat"))
            materialHandle = assets.LoadMaterial(rawMaterial.c_str());
    }

    SceneObject object = {};
    object.mesh = mesh;
    object.material = materialHandle != 0 ? materialHandle : assets.CreateMaterial(ParseMaterial(block));
    object.transform = transform;

    const uint32_t objectId = scene.CreateObject(object);
    if (mainObjectId == 0)
        mainObjectId = objectId;

    const std::vector<std::string> colliders = FindLevelProperties(block, "Collider");
    for (const std::string& colliderValue : colliders)
    {
        LevelBlock colliderBlock = {};
        colliderBlock.type = LevelBlockType::Collider;
        colliderBlock.line = block.line;
        colliderBlock.properties.push_back({"Spec", colliderValue});
        colliderBlock.properties.push_back({
            "Position",
            std::to_string(transform.position.x) + "," + std::to_string(transform.position.y) + "," + std::to_string(transform.position.z)
        });
        colliderBlock.properties.push_back({
            "Scale",
            std::to_string(std::fabs(transform.scale.x)) + "," + std::to_string(std::fabs(transform.scale.y)) + "," +
            std::to_string(std::fabs(transform.scale.z))
        });

        Transform colliderRoot = {};
        colliderRoot.scale = { 1.0f, 1.0f, 1.0f };
        if (!InstantiateColliderBlock(colliderBlock, colliderRoot))
            return false;
    }

    return true;
}

bool World::InstantiateEntityBlock(const LevelBlock& block, const Transform& rootTransform)
{
    const std::string* classValue = FindLevelProperty(block, "Class");
    if (!classValue)
        return false;

    const std::string className = textparse::Unquote(*classValue);
    Entity* entity = entityFactories.Create(className, entities, gameContext);
    if (!entity)
    {
        EngineLogf("No entity factory registered for class %s", className.c_str());
        return false;
    }

    Transform local = {};
    ParseTransformProperties(block, local);
    const Transform transform = ComposeTransforms(rootTransform, local);

    entity->Rotation = transform.rotation;
    entity->Tags = ParseTags(FindLevelProperty(block, "Tags"));
    entity->Teleport(transform.position);

    for (const LevelProperty& property : block.properties)
    {
        if (property.key == "Class" || property.key == "Position" || property.key == "Location" || property.key ==
            "Rotation" || property.key == "Tags")
            continue;

        entity->ApplyProperty(property.key, property.value);
    }

    return true;
}

bool World::InstantiateColliderBlock(const LevelBlock& block, const Transform& rootTransform)
{
    std::string typeName;
    Transform local = {};
    ParseTransformProperties(block, local);
    const Transform transform = ComposeTransforms(rootTransform, local);

    Vector3 position = transform.position;
    Vector3 halfExtents = {0.5f, 0.5f, 0.5f};
    float radius = 0.5f;
    float halfHeight = 0.5f;
    bool isTrigger = false;
    const bool hasScale = FindLevelProperty(block, "Scale") != nullptr;
    const Vector3 scale = {
        std::fabs(transform.scale.x),
        std::fabs(transform.scale.y),
        std::fabs(transform.scale.z)
    };
    const std::string* halfExtentsValue = FindLevelProperty(block, "HalfExtents");
    const std::string* radiusValue = FindLevelProperty(block, "Radius");
    const std::string* halfHeightValue = FindLevelProperty(block, "HalfHeight");

    if (const std::string* spec = FindLevelProperty(block, "Spec"))
    {
        const std::vector<std::string> parts = textparse::SplitList(*spec);
        if (parts.empty())
            return false;

        typeName = parts[0];
        if (typeName == "Box" && parts.size() >= 4)
        {
            halfExtents = {
                std::strtof(parts[1].c_str(), nullptr),
                std::strtof(parts[2].c_str(), nullptr),
                std::strtof(parts[3].c_str(), nullptr)
            };
        }
        else if (typeName == "Sphere" && parts.size() >= 2)
        {
            radius = std::strtof(parts[1].c_str(), nullptr);
        }
        else if (typeName == "Capsule" && parts.size() >= 3)
        {
            radius = std::strtof(parts[1].c_str(), nullptr);
            halfHeight = std::strtof(parts[2].c_str(), nullptr);
        }
        else
        {
            return false;
        }
    }
    else
    {
        const std::string* typeValue = FindLevelProperty(block, "Type");
        if (!typeValue)
            return false;

        typeName = textparse::Unquote(*typeValue);

        if (halfExtentsValue)
            halfExtents = textparse::ParseVector3(*halfExtentsValue, halfExtents);

        if (radiusValue)
            radius = textparse::ParseFloat(*radiusValue, radius);

        if (halfHeightValue)
            halfHeight = textparse::ParseFloat(*halfHeightValue, halfHeight);

        if (const std::string* triggerValue = FindLevelProperty(block, "IsTrigger"))
            isTrigger = textparse::ParseBool(*triggerValue, false);
    }

    if (typeName == "Box")
    {
        if (!halfExtentsValue && hasScale)
            halfExtents = {scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f};
        collision.AddBox(position, halfExtents, isTrigger);
        return true;
    }
    if (typeName == "Sphere")
    {
        if (!radiusValue)
        {
            if (halfExtentsValue)
            {
                const Vector3 parsedHalfExtents = textparse::ParseVector3(*halfExtentsValue, halfExtents);
                radius = std::max(parsedHalfExtents.x, std::max(parsedHalfExtents.y, parsedHalfExtents.z));
            }
            else if (hasScale)
            {
                radius = std::max(scale.x, std::max(scale.y, scale.z)) * 0.5f;
            }
        }
        collision.AddSphere(position, radius, isTrigger);
        return true;
    }
    if (typeName == "Capsule")
    {
        if (!radiusValue || !halfHeightValue)
        {
            const Vector3 capsuleScale = halfExtentsValue
                                             ? textparse::ParseVector3(*halfExtentsValue, {
                                                                     scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f
                                                                 })
                                             : Vector3{scale.x * 0.5f, scale.y * 0.5f, scale.z * 0.5f};
            if (!radiusValue)
                radius = std::max(capsuleScale.x, capsuleScale.z);
            if (!halfHeightValue)
                halfHeight = capsuleScale.y;
        }
        collision.AddCapsule(position, radius, halfHeight, isTrigger);
        return true;
    }

    return false;
}

bool World::InstantiatePrefabBlock(const LevelBlock& block, const Transform& rootTransform)
{
    const std::string* fileValue = FindLevelProperty(block, "File");
    if (!fileValue)
        return false;

    std::string error;
    const std::vector<LevelBlock>* prefabBlocks = prefabs.LoadPrefab(textparse::Unquote(*fileValue).c_str(), &error);
    if (!prefabBlocks)
    {
        EngineLogf("Failed to load prefab %s: %s", fileValue->c_str(), error.c_str());
        return false;
    }

    Transform local = {};
    ParseTransformProperties(block, local);
    const Transform transform = ComposeTransforms(rootTransform, local);
    return InstantiateMap(*prefabBlocks, transform);
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
    scene.SetEntityManager(&entities);
}

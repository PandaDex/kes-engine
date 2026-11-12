#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "engine_types.h"
#include "collision_world.h"
#include "guid.h"
#include "material.h"
#include "mesh.h"

struct GameContext;
class EntityManager;

// Tag of the entity World spawns as the player. Triggers react to it by default.
inline constexpr const char* kPlayerTag = "Player";

// Geometry a map gives to a solid (brush) entity, in engine space relative to
// the entity's Position. Built by World::LoadMap from the compiled map.
struct MapEntityGeometry
{
    Vector3 boundsMin;
    Vector3 boundsMax;
    struct Mesh
    {
        MeshHandle mesh = 0;
        MaterialHandle material = 0;
    };
    std::vector<Mesh> meshes;
    using Hull = ConvexCollider;
    std::vector<Hull> hulls;
};

class Entity
{
public:
    virtual ~Entity() = default;

    Vector3 Position = {};
    Vector3 Rotation = {};
    std::vector<std::string> Tags;

    void SetGameContext(GameContext* ctx) { game = ctx; }
    uint64_t GetEntityId() const { return entityId; }
    // Stable identity: the editor's GUID for map entities, random for ones spawned at runtime.
    // Change it with EntityManager::SetEntityGuid.
    const Guid& GetGuid() const { return guid; }

    virtual void Start() {}
    virtual void Update(float dt) { (void)dt; }
    virtual void FixedUpdate(float fixedDt) { (void)fixedDt; }
    virtual void Teleport(const Vector3& newPosition);

    virtual bool ApplyProperty(const std::string& name, const std::string& value);
    // Solid map entities receive their brush geometry before Start(). Volumes
    // (triggers, fog) usually only need the bounds, which also arrive as "Size".
    virtual void ApplyMapGeometry(const MapEntityGeometry& geometry) { (void)geometry; }
    // Colliders of this entity, skipped by its built-in gravity.
    virtual bool OwnsCollider(ColliderHandle handle) const { (void)handle; return false; }

    bool HasTag(const std::string& tag) const
    {
        for (const std::string& currentTag : Tags)
        {
            if (currentTag == tag)
                return true;
        }
        return false;
    }

    bool IsPendingDestroy() const { return pendingDestroy; }
    void Destroy() { pendingDestroy = true; }

protected:
    virtual void OnDestroy() {}
    // MapScript: queues the inputs linked to `output` of this entity (see map_script.h).
    void FireOutput(std::string_view output, Entity* activator = nullptr, std::string_view value = {});
    // Dynamic, Density, Friction, Restitution, RollingResistance map properties of physics entities.
    static bool ApplyRigidBodyProperty(RigidBodyDesc& desc, const std::string& name, const std::string& value);

    GameContext* game = nullptr;

private:
    friend class EntityManager;

    void RunBuiltinPhysics(float fixedDt);
    void RunDestroy()
    {
        OnDestroy();
    }

    bool pendingDestroy = false;
    bool destroyRan = false;   // OnDestroy runs once
    uint64_t entityId = 0;
    Guid guid;
    bool gravityEnabled = false;
    float verticalVelocity = 0.0f;
    float gravityAcceleration = 18.0f;
};

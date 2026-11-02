#pragma once
#include <string>
#include <vector>
#include "engine_types.h"
#include "collision_world.h"
#include "material.h"
#include "mesh.h"

struct GameContext;
class EntityManager;

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
    struct Hull
    {
        std::vector<CollisionPlane> planes;
        Vector3 boundsMin;
        Vector3 boundsMax;
    };
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

    virtual void Start() {}
    virtual void Update(float dt) { (void)dt; }
    virtual void FixedUpdate(float fixedDt) { (void)fixedDt; }
    virtual void Teleport(const Vector3& newPosition);

    virtual bool ApplyProperty(const std::string& name, const std::string& value);
    // Solid map entities receive their brush geometry before Start(). Volumes
    // (triggers, fog) usually only need the bounds, which also arrive as "Size".
    virtual void ApplyMapGeometry(const MapEntityGeometry& geometry) { (void)geometry; }

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
    bool gravityEnabled = false;
    float verticalVelocity = 0.0f;
    float gravityAcceleration = 18.0f;
};

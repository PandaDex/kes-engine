#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "engine_types.h"

// Collision queries, rigid bodies and the character mover, built on Box3D.
// Box3D stays an implementation detail of collision_world.cpp.

using ColliderHandle = uint32_t;

enum class ColliderType
{
    Box,
    Sphere,
    Capsule,
    Convex   // convex hull from planes (map brushes)
};

// Static never moves on its own, Kinematic moves where it is told and pushes
// dynamic bodies, Dynamic is simulated.
enum class ColliderMotion
{
    Static,
    Kinematic,
    Dynamic
};

// Plane in the body's local space: dot(normal, p) == dist, normal points outward.
struct CollisionPlane
{
    Vector3 normal = { 0.0f, 1.0f, 0.0f };
    float dist = 0.0f;
};

struct ConvexCollider
{
    std::vector<CollisionPlane> planes;
    Vector3 boundsMin = {};   // local AABB of the hull, used if the planes don't form a valid hull
    Vector3 boundsMax = {};
};

// One shape of a compound body, in the body's local space. Box uses halfExtents,
// Sphere radius, Capsule radius and halfHeight along its local Y.
struct RigidShape
{
    ColliderType type = ColliderType::Box;
    Vector3 center = {};
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // quaternion x, y, z, w
    Vector3 halfExtents = { 0.5f, 0.5f, 0.5f };
    float radius = 0.5f;
    float halfHeight = 0.5f;
};

struct RigidBodyDesc
{
    ColliderMotion motion = ColliderMotion::Dynamic;
    Vector3 position = {};
    Vector3 rotation = {};      // radians, same convention as Entity::Rotation
    float density = 500.0f;     // kg per cubic meter
    float friction = 0.6f;
    float restitution = 0.0f;
    float rollingResistance = 0.1f;   // spheres: without it they roll forever on a flat floor
};

struct CollisionHit
{
    bool hit = false;
    ColliderHandle other = 0;
    Vector3 normal = {};      // surface normal at the contact, facing the moving shape
    float fraction = 1.0f;    // part of the sweep that was free of contact
};

struct Ray
{
    Vector3 origin = {};
    Vector3 direction = { 0.0f, 0.0f, 1.0f };
};

struct RaycastQuery
{
    float maxDistance = 1000.0f;
    ColliderHandle ignore = 0;
    bool includeTriggers = false;
};

struct RaycastHit
{
    bool hit = false;
    ColliderHandle other = 0;
    ColliderType type = ColliderType::Box;
    Vector3 point = {};
    Vector3 normal = {};
    float distance = 0.0f;
};

class CollisionWorld
{
public:
    CollisionWorld();
    ~CollisionWorld();
    CollisionWorld(const CollisionWorld&) = delete;
    CollisionWorld& operator=(const CollisionWorld&) = delete;

    ColliderHandle AddBox(const Vector3& position, const Vector3& halfExtents, bool isTrigger = false, const Vector3& offset = {});
    ColliderHandle AddSphere(const Vector3& position, float radius, bool isTrigger = false, const Vector3& offset = {});
    // Vertical capsule. A character's capsule is Kinematic so it pushes dynamic bodies.
    ColliderHandle AddCapsule(const Vector3& position, float radius, float halfHeight, bool isTrigger = false,
                              const Vector3& offset = {}, ColliderMotion motion = ColliderMotion::Static);
    // Convex hull: the intersection of the half-spaces behind `planes` (local to `position`).
    ColliderHandle AddConvex(const Vector3& position, const std::vector<CollisionPlane>& planes,
                             const Vector3& boundsMin, const Vector3& boundsMax, bool isTrigger = false);

    // One body made of `hulls`, local to desc.position.
    ColliderHandle AddRigidHulls(const std::vector<ConvexCollider>& hulls, const RigidBodyDesc& desc);
    ColliderHandle AddRigidBox(const Vector3& halfExtents, const Vector3& offset, const RigidBodyDesc& desc);
    ColliderHandle AddRigidSphere(float radius, const Vector3& offset, const RigidBodyDesc& desc);
    ColliderHandle AddRigidShapes(const std::vector<RigidShape>& shapes, const RigidBodyDesc& desc);

    bool Remove(ColliderHandle handle);
    bool Exists(ColliderHandle handle) const;
    void SetActive(ColliderHandle handle, bool active);

    // Teleports the body, stopping it if it is dynamic.
    void SetPosition(ColliderHandle handle, const Vector3& position);
    // Kinematic bodies: reaches `position` over `timeStep` at a velocity, so it pushes what it meets.
    void SetTargetPosition(ColliderHandle handle, const Vector3& position, float timeStep);
    // Places the body at parent * local, where parent is a position and Euler rotation (as on
    // entities) and local a position and quaternion (x, y, z, w) in the parent's space.
    // Kinematic bodies move there over timeStep (> 0) so they push what they meet; otherwise it teleports.
    void SetTargetPose(ColliderHandle handle, const Vector3& parentPosition, const Vector3& parentRotation,
                       const Vector3& localPosition, const float localRotation[4], float timeStep);
    Vector3 GetWorldPosition(ColliderHandle handle) const;

    void Step(float timeStep);
    // Pose of a dynamic body between the last two steps, `alpha` 0 = previous, 1 = latest.
    bool GetBodyPose(ColliderHandle handle, float alpha, Vector3& outPosition, Vector3& outRotation) const;
    void ApplyImpulse(ColliderHandle handle, const Vector3& impulse);
    void SetLinearVelocity(ColliderHandle handle, const Vector3& velocity);
    Vector3 GetLinearVelocity(ColliderHandle handle) const;

    // Overlap tests against solid bodies; triggers don't count.
    bool CheckBox(const Vector3& position, const Vector3& halfExtents, ColliderHandle ignore = 0) const;
    bool CheckSphere(const Vector3& position, float radius, ColliderHandle ignore = 0) const;
    bool CheckCapsule(const Vector3& position, float radius, float halfHeight, ColliderHandle ignore = 0) const;

    CollisionHit SweepCapsule(const Vector3& from, const Vector3& to, float radius, float halfHeight, ColliderHandle ignore = 0) const;
    // Rays starting inside a shape hit it at distance 0.
    bool Raycast(const Ray& ray, RaycastHit& outHit, const RaycastQuery& query = {}) const;
    std::size_t RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits, const RaycastQuery& query = {}) const;

    // Moves a vertical capsule by `delta`, sliding along what blocks it and out
    // of what it overlaps. Returns the new position. Dynamic bodies are soft:
    // they don't stop the capsule, its kinematic body pushes them instead.
    Vector3 MoveCapsule(const Vector3& position, const Vector3& delta, float radius, float halfHeight, ColliderHandle ignore = 0) const;

    void Clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

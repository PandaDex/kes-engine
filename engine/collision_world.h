#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "engine_types.h"

using ColliderHandle = uint32_t;

enum class ColliderType
{
    Box,
    Sphere,
    Capsule
};

struct BoxCollider
{
    Vector3 halfExtents = { 0.5f, 0.5f, 0.5f };
};

struct SphereCollider
{
    float radius = 0.5f;
};

struct CapsuleCollider
{
    float radius = 0.5f;
    float halfHeight = 0.5f;
};

struct CollisionBody
{
    ColliderHandle handle = 0;
    ColliderType type = ColliderType::Box;
    Vector3 position = {};
    Vector3 offset = {};
    BoxCollider box = {};
    SphereCollider sphere = {};
    CapsuleCollider capsule = {};
    bool isTrigger = false;
    bool active = true;
};

struct CollisionHit
{
    bool hit = false;
    ColliderHandle other = 0;
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
    ColliderHandle AddBox(const Vector3& position, const Vector3& halfExtents, bool isTrigger = false, const Vector3& offset = {});
    ColliderHandle AddSphere(const Vector3& position, float radius, bool isTrigger = false, const Vector3& offset = {});
    ColliderHandle AddCapsule(const Vector3& position, float radius, float halfHeight, bool isTrigger = false, const Vector3& offset = {});

    bool Remove(ColliderHandle handle);
    CollisionBody* Find(ColliderHandle handle);
    const CollisionBody* Find(ColliderHandle handle) const;

    void SetPosition(ColliderHandle handle, const Vector3& position);
    Vector3 GetWorldPosition(ColliderHandle handle) const;

    bool CheckBox(const Vector3& position, const Vector3& halfExtents, ColliderHandle ignore = 0) const;
    bool CheckSphere(const Vector3& position, float radius, ColliderHandle ignore = 0) const;
    bool CheckCapsule(const Vector3& position, float radius, float halfHeight, ColliderHandle ignore = 0) const;

    CollisionHit SweepCapsule(const Vector3& from, const Vector3& to, float radius, float halfHeight, ColliderHandle ignore = 0, int steps = 8) const;
    bool Raycast(const Ray& ray, RaycastHit& outHit, const RaycastQuery& query = {}) const;
    std::size_t RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits, const RaycastQuery& query = {}) const;

    void Clear();

private:
    bool OverlapsBody(const CollisionBody& probe, ColliderHandle ignore) const;
    bool OverlapsPair(const CollisionBody& a, const CollisionBody& b) const;

private:
    std::vector<CollisionBody> bodies;
    ColliderHandle nextHandle = 1;
};

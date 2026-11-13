#include "collision_world.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <box3d/box3d.h>
#include "logger.h"
#include "profiler.h"

namespace
{
    // A shape only matches a query when its mask has the query's category too,
    // so triggers accept kCategoryQuery and nothing that collides.
    constexpr uint64_t kCategorySolid = 1u << 0;     // static and kinematic
    constexpr uint64_t kCategoryDynamic = 1u << 1;
    constexpr uint64_t kCategoryTrigger = 1u << 2;
    constexpr uint64_t kCategoryQuery = 1u << 3;

    constexpr int kSubStepCount = 4;
    constexpr float kGravity = -9.81f;
    constexpr float kMinHalfExtent = 0.005f;   // Box3D rejects degenerate shapes
    constexpr float kDynamicPushLimit = 0.02f; // how far a dynamic body may push the mover per solve
    constexpr int kMaxMoverPlanes = 32;
    constexpr int kMaxMoverIterations = 5;

    struct Pose
    {
        b3Pos position = {};
        b3Quat rotation = b3Quat_identity;
    };

    b3Vec3 ToB3(const Vector3& v) { return { v.x, v.y, v.z }; }
    Vector3 FromB3(b3Vec3 v) { return { v.x, v.y, v.z }; }

    // Entity rotations are applied X, then Y, then Z (render::TransformToMatrix).
    b3Quat QuatFromEuler(const Vector3& euler)
    {
        const float cx = std::cos(euler.x * 0.5f), sx = std::sin(euler.x * 0.5f);
        const float cy = std::cos(euler.y * 0.5f), sy = std::sin(euler.y * 0.5f);
        const float cz = std::cos(euler.z * 0.5f), sz = std::sin(euler.z * 0.5f);
        b3Quat q;
        q.v.x = cz * cy * sx - sz * sy * cx;
        q.v.y = cz * sy * cx + sz * cy * sx;
        q.v.z = sz * cy * cx - cz * sy * sx;
        q.s = cz * cy * cx + sz * sy * sx;
        return q;
    }

    Vector3 EulerFromQuat(b3Quat q)
    {
        const float x = q.v.x, y = q.v.y, z = q.v.z, w = q.s;
        const float m00 = 1.0f - 2.0f * (y * y + z * z);
        const float m10 = 2.0f * (x * y + z * w);
        const float m20 = 2.0f * (x * z - y * w);
        const float m21 = 2.0f * (y * z + x * w);
        const float m22 = 1.0f - 2.0f * (x * x + y * y);
        const float yaw = std::asin(ClampFloat(-m20, -1.0f, 1.0f));
        if (std::fabs(m20) > 0.9999f)
        {
            // Gimbal lock: X and Z turn about the same axis, so Z takes all of it.
            const float m01 = 2.0f * (x * y - z * w);
            const float m11 = 1.0f - 2.0f * (x * x + z * z);
            return { 0.0f, yaw, std::atan2(-m01, m11) };
        }
        return { std::atan2(m21, m22), yaw, std::atan2(m10, m00) };
    }

    b3Quat BlendQuat(b3Quat from, b3Quat to, float t)
    {
        const float dot = from.v.x * to.v.x + from.v.y * to.v.y + from.v.z * to.v.z + from.s * to.s;
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        b3Quat q;
        q.v.x = from.v.x + (to.v.x * sign - from.v.x) * t;
        q.v.y = from.v.y + (to.v.y * sign - from.v.y) * t;
        q.v.z = from.v.z + (to.v.z * sign - from.v.z) * t;
        q.s = from.s + (to.s * sign - from.s) * t;
        const float length = std::sqrt(q.v.x * q.v.x + q.v.y * q.v.y + q.v.z * q.v.z + q.s * q.s);
        if (length <= 1e-6f)
            return to;
        q.v.x /= length;
        q.v.y /= length;
        q.v.z /= length;
        q.s /= length;
        return q;
    }

    ColliderHandle HandleOf(b3ShapeId shape)
    {
        return static_cast<ColliderHandle>(reinterpret_cast<uintptr_t>(b3Shape_GetUserData(shape)));
    }

    b3QueryFilter MakeQueryFilter(bool includeTriggers)
    {
        b3QueryFilter filter = b3DefaultQueryFilter();
        filter.categoryBits = kCategoryQuery;
        filter.maskBits = kCategorySolid | kCategoryDynamic | (includeTriggers ? kCategoryTrigger : 0u);
        return filter;
    }

    // Corners of the hull: every point where three planes meet that no plane cuts away.
    std::vector<b3Vec3> HullPointsFromPlanes(const std::vector<CollisionPlane>& planes)
    {
        struct Plane
        {
            double x, y, z, d;
        };
        std::vector<Plane> p;
        p.reserve(planes.size());
        for (const CollisionPlane& plane : planes)
            p.push_back({ plane.normal.x, plane.normal.y, plane.normal.z, plane.dist });

        std::vector<b3Vec3> points;
        const size_t count = p.size();
        for (size_t i = 0; i < count; ++i)
        {
            for (size_t j = i + 1; j < count; ++j)
            {
                const double cx = p[i].y * p[j].z - p[i].z * p[j].y;
                const double cy = p[i].z * p[j].x - p[i].x * p[j].z;
                const double cz = p[i].x * p[j].y - p[i].y * p[j].x;
                for (size_t k = j + 1; k < count; ++k)
                {
                    const double det = cx * p[k].x + cy * p[k].y + cz * p[k].z;
                    if (std::fabs(det) < 1e-9)
                        continue;

                    // Cramer's rule: (d_i (n_j x n_k) + d_j (n_k x n_i) + d_k (n_i x n_j)) / det
                    const double jkx = p[j].y * p[k].z - p[j].z * p[k].y;
                    const double jky = p[j].z * p[k].x - p[j].x * p[k].z;
                    const double jkz = p[j].x * p[k].y - p[j].y * p[k].x;
                    const double kix = p[k].y * p[i].z - p[k].z * p[i].y;
                    const double kiy = p[k].z * p[i].x - p[k].x * p[i].z;
                    const double kiz = p[k].x * p[i].y - p[k].y * p[i].x;
                    const double x = (p[i].d * jkx + p[j].d * kix + p[k].d * cx) / det;
                    const double y = (p[i].d * jky + p[j].d * kiy + p[k].d * cy) / det;
                    const double z = (p[i].d * jkz + p[j].d * kiz + p[k].d * cz) / det;

                    bool inside = true;
                    for (const Plane& plane : p)
                    {
                        if (plane.x * x + plane.y * y + plane.z * z - plane.d > 1e-4)
                        {
                            inside = false;
                            break;
                        }
                    }
                    if (!inside)
                        continue;

                    const b3Vec3 point = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
                    const bool duplicate = std::any_of(points.begin(), points.end(), [&point](const b3Vec3& other)
                    {
                        return std::fabs(other.x - point.x) + std::fabs(other.y - point.y) + std::fabs(other.z - point.z) < 1e-4f;
                    });
                    if (!duplicate)
                        points.push_back(point);
                }
            }
        }
        return points;
    }

    struct CapsuleProxy
    {
        b3Vec3 points[2];
        b3ShapeProxy proxy;

        CapsuleProxy(float radius, float halfHeight)
        {
            points[0] = { 0.0f, -halfHeight, 0.0f };
            points[1] = { 0.0f, halfHeight, 0.0f };
            proxy = { points, 2, radius };
        }
    };

    struct OverlapContext
    {
        ColliderHandle ignore = 0;
        bool found = false;
        ColliderHandle first = 0;
        b3ShapeId shape = {};
    };

    bool OverlapCallback(b3ShapeId shape, void* context)
    {
        auto* overlap = static_cast<OverlapContext*>(context);
        const ColliderHandle handle = HandleOf(shape);
        if (handle == overlap->ignore)
            return true;
        overlap->found = true;
        overlap->first = handle;
        overlap->shape = shape;
        return false;
    }

    struct ClosestCastContext
    {
        ColliderHandle ignore = 0;
        bool found = false;
        float fraction = 1.0f;
        ColliderHandle other = 0;
        Vector3 point = {};
        Vector3 normal = {};
    };

    float ClosestCastCallback(b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context)
    {
        auto* cast = static_cast<ClosestCastContext*>(context);
        const ColliderHandle handle = HandleOf(shape);
        if (handle == cast->ignore)
            return -1.0f;
        if (!cast->found || fraction < cast->fraction)
        {
            cast->found = true;
            cast->fraction = fraction;
            cast->other = handle;
            cast->point = FromB3(point);
            cast->normal = FromB3(normal);
        }
        return fraction;
    }
}

struct CollisionWorld::Impl
{
    struct Entry
    {
        b3BodyId body = {};
        ColliderType type = ColliderType::Box;
        ColliderMotion motion = ColliderMotion::Static;
        Vector3 offset = {};
        Pose previous;   // dynamic bodies: the last two steps, for interpolation
        Pose current;
    };

    b3WorldId world = {};
    std::unordered_map<ColliderHandle, Entry> entries;
    std::vector<ColliderHandle> dynamicBodies;
    ColliderHandle nextHandle = 1;

    void CreateWorld()
    {
        b3WorldDef def = b3DefaultWorldDef();
        def.gravity = { 0.0f, kGravity, 0.0f };
        world = b3CreateWorld(&def);
    }

    Entry* Find(ColliderHandle handle)
    {
        const auto found = entries.find(handle);
        return found != entries.end() ? &found->second : nullptr;
    }

    const Entry* Find(ColliderHandle handle) const
    {
        const auto found = entries.find(handle);
        return found != entries.end() ? &found->second : nullptr;
    }

    ColliderHandle CreateBody(ColliderType type, const RigidBodyDesc& desc, const Vector3& offset)
    {
        const ColliderHandle handle = nextHandle++;
        b3BodyDef def = b3DefaultBodyDef();
        def.type = desc.motion == ColliderMotion::Dynamic ? b3_dynamicBody
                   : desc.motion == ColliderMotion::Kinematic ? b3_kinematicBody
                                                              : b3_staticBody;
        def.position = b3ToPos(ToB3(desc.position));
        def.rotation = QuatFromEuler(desc.rotation);

        Entry entry;
        entry.body = b3CreateBody(world, &def);
        entry.type = type;
        entry.motion = desc.motion;
        entry.offset = offset;
        entry.current = { def.position, def.rotation };
        entry.previous = entry.current;
        entries[handle] = entry;
        if (desc.motion == ColliderMotion::Dynamic)
            dynamicBodies.push_back(handle);
        return handle;
    }

    static b3ShapeDef MakeShapeDef(ColliderHandle handle, const RigidBodyDesc& desc, bool isTrigger)
    {
        b3ShapeDef def = b3DefaultShapeDef();
        def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
        def.density = desc.density;
        def.baseMaterial.friction = desc.friction;
        def.baseMaterial.restitution = desc.restitution;
        def.baseMaterial.rollingResistance = desc.rollingResistance;
        if (isTrigger)
        {
            def.filter.categoryBits = kCategoryTrigger;
            def.filter.maskBits = kCategoryQuery;
        }
        else
        {
            def.filter.categoryBits = desc.motion == ColliderMotion::Dynamic ? kCategoryDynamic : kCategorySolid;
            def.filter.maskBits = kCategorySolid | kCategoryDynamic | kCategoryQuery;
        }
        return def;
    }

    void AddBoxShape(ColliderHandle handle, const Vector3& halfExtents, const Vector3& offset, const RigidBodyDesc& desc, bool isTrigger)
    {
        const b3ShapeDef def = MakeShapeDef(handle, desc, isTrigger);
        const b3BoxHull box = b3MakeOffsetBoxHull(std::max(halfExtents.x, kMinHalfExtent), std::max(halfExtents.y, kMinHalfExtent),
                                                  std::max(halfExtents.z, kMinHalfExtent), ToB3(offset));
        b3CreateHullShape(entries[handle].body, &def, &box.base);
    }

    void AddHullShape(ColliderHandle handle, const ConvexCollider& collider, const RigidBodyDesc& desc, bool isTrigger)
    {
        const std::vector<b3Vec3> points = HullPointsFromPlanes(collider.planes);
        b3HullData* hull = points.size() >= 4 ? b3CreateHull(points.data(), static_cast<int>(points.size()), B3_MAX_HULL_VERTICES) : nullptr;
        if (!hull)
        {
            EngineLogf("Convex collider with %zu planes has no valid hull; using its bounds.", collider.planes.size());
            const Vector3 half = (collider.boundsMax - collider.boundsMin) * 0.5f;
            AddBoxShape(handle, half, (collider.boundsMax + collider.boundsMin) * 0.5f, desc, isTrigger);
            return;
        }
        const b3ShapeDef def = MakeShapeDef(handle, desc, isTrigger);
        b3CreateHullShape(entries[handle].body, &def, hull);
        b3DestroyHull(hull);
    }

    void AddSphereShape(ColliderHandle handle, float radius, const Vector3& offset, const RigidBodyDesc& desc, bool isTrigger)
    {
        const b3ShapeDef def = MakeShapeDef(handle, desc, isTrigger);
        const b3Sphere sphere = { ToB3(offset), std::max(radius, kMinHalfExtent) };
        b3CreateSphereShape(entries[handle].body, &def, &sphere);
    }

    bool Overlaps(const Vector3& position, const b3ShapeProxy& proxy, ColliderHandle ignore) const
    {
        OverlapContext context;
        context.ignore = ignore;
        b3World_OverlapShape(world, b3ToPos(ToB3(position)), &proxy, MakeQueryFilter(false), OverlapCallback, &context);
        return context.found;
    }
};

CollisionWorld::CollisionWorld() : impl(std::make_unique<Impl>())
{
    impl->CreateWorld();
}

CollisionWorld::~CollisionWorld()
{
    b3DestroyWorld(impl->world);
}

ColliderHandle CollisionWorld::AddBox(const Vector3& position, const Vector3& halfExtents, bool isTrigger, const Vector3& offset)
{
    RigidBodyDesc desc;
    desc.motion = ColliderMotion::Static;
    desc.position = position;
    const ColliderHandle handle = impl->CreateBody(ColliderType::Box, desc, offset);
    impl->AddBoxShape(handle, halfExtents, offset, desc, isTrigger);
    return handle;
}

ColliderHandle CollisionWorld::AddSphere(const Vector3& position, float radius, bool isTrigger, const Vector3& offset)
{
    RigidBodyDesc desc;
    desc.motion = ColliderMotion::Static;
    desc.position = position;
    const ColliderHandle handle = impl->CreateBody(ColliderType::Sphere, desc, offset);
    impl->AddSphereShape(handle, radius, offset, desc, isTrigger);
    return handle;
}

ColliderHandle CollisionWorld::AddCapsule(const Vector3& position, float radius, float halfHeight, bool isTrigger, const Vector3& offset,
                                          ColliderMotion motion)
{
    RigidBodyDesc desc;
    desc.motion = motion;
    desc.position = position;
    const ColliderHandle handle = impl->CreateBody(ColliderType::Capsule, desc, offset);
    const b3ShapeDef def = Impl::MakeShapeDef(handle, desc, isTrigger);
    const b3Capsule capsule = { { offset.x, offset.y - halfHeight, offset.z }, { offset.x, offset.y + halfHeight, offset.z }, radius };
    b3CreateCapsuleShape(impl->entries[handle].body, &def, &capsule);
    return handle;
}

ColliderHandle CollisionWorld::AddConvex(const Vector3& position, const std::vector<CollisionPlane>& planes,
                                         const Vector3& boundsMin, const Vector3& boundsMax, bool isTrigger)
{
    RigidBodyDesc desc;
    desc.motion = ColliderMotion::Static;
    desc.position = position;
    const ColliderHandle handle = impl->CreateBody(ColliderType::Convex, desc, {});
    impl->AddHullShape(handle, { planes, boundsMin, boundsMax }, desc, isTrigger);
    return handle;
}

ColliderHandle CollisionWorld::AddRigidHulls(const std::vector<ConvexCollider>& hulls, const RigidBodyDesc& desc)
{
    const ColliderHandle handle = impl->CreateBody(ColliderType::Convex, desc, {});
    for (const ConvexCollider& hull : hulls)
        impl->AddHullShape(handle, hull, desc, false);
    return handle;
}

ColliderHandle CollisionWorld::AddRigidBox(const Vector3& halfExtents, const Vector3& offset, const RigidBodyDesc& desc)
{
    const ColliderHandle handle = impl->CreateBody(ColliderType::Box, desc, {});
    impl->AddBoxShape(handle, halfExtents, offset, desc, false);
    return handle;
}

ColliderHandle CollisionWorld::AddRigidSphere(float radius, const Vector3& offset, const RigidBodyDesc& desc)
{
    const ColliderHandle handle = impl->CreateBody(ColliderType::Sphere, desc, {});
    impl->AddSphereShape(handle, radius, offset, desc, false);
    return handle;
}

ColliderHandle CollisionWorld::AddRigidShapes(const std::vector<RigidShape>& shapes, const RigidBodyDesc& desc)
{
    if (shapes.empty())
        return 0;
    const ColliderType type = shapes.size() == 1 ? shapes.front().type : ColliderType::Convex;
    const ColliderHandle handle = impl->CreateBody(type, desc, {});
    const b3ShapeDef def = Impl::MakeShapeDef(handle, desc, false);
    const b3BodyId body = impl->entries[handle].body;
    for (const RigidShape& shape : shapes)
    {
        b3Quat q = { { shape.rotation[0], shape.rotation[1], shape.rotation[2] }, shape.rotation[3] };
        q = b3NormalizeQuat(q);
        if (shape.type == ColliderType::Sphere)
        {
            const b3Sphere sphere = { ToB3(shape.center), std::max(shape.radius, kMinHalfExtent) };
            b3CreateSphereShape(body, &def, &sphere);
        }
        else if (shape.type == ColliderType::Capsule)
        {
            const b3Vec3 axis = b3RotateVector(q, { 0.0f, std::max(shape.halfHeight, 0.0f), 0.0f });
            const b3Vec3 center = ToB3(shape.center);
            const b3Capsule capsule = { b3Sub(center, axis), b3Add(center, axis), std::max(shape.radius, kMinHalfExtent) };
            b3CreateCapsuleShape(body, &def, &capsule);
        }
        else
        {
            const b3Transform transform = { ToB3(shape.center), q };
            const b3BoxHull box = b3MakeTransformedBoxHull(std::max(shape.halfExtents.x, kMinHalfExtent),
                                                           std::max(shape.halfExtents.y, kMinHalfExtent),
                                                           std::max(shape.halfExtents.z, kMinHalfExtent), transform);
            b3CreateHullShape(body, &def, &box.base);
        }
    }
    return handle;
}

bool CollisionWorld::Remove(ColliderHandle handle)
{
    Impl::Entry* entry = impl->Find(handle);
    if (!entry)
        return false;
    b3DestroyBody(entry->body);
    if (entry->motion == ColliderMotion::Dynamic)
    {
        std::vector<ColliderHandle>& bodies = impl->dynamicBodies;
        bodies.erase(std::remove(bodies.begin(), bodies.end(), handle), bodies.end());
    }
    impl->entries.erase(handle);
    return true;
}

bool CollisionWorld::Exists(ColliderHandle handle) const
{
    return impl->Find(handle) != nullptr;
}

void CollisionWorld::SetActive(ColliderHandle handle, bool active)
{
    if (Impl::Entry* entry = impl->Find(handle))
    {
        if (active)
            b3Body_Enable(entry->body);
        else
            b3Body_Disable(entry->body);
    }
}

void CollisionWorld::SetPosition(ColliderHandle handle, const Vector3& position)
{
    Impl::Entry* entry = impl->Find(handle);
    if (!entry)
        return;

    const b3Quat rotation = b3Body_GetRotation(entry->body);
    b3Body_SetTransform(entry->body, b3ToPos(ToB3(position)), rotation);
    if (entry->motion == ColliderMotion::Dynamic)
    {
        b3Body_SetLinearVelocity(entry->body, { 0.0f, 0.0f, 0.0f });
        b3Body_SetAngularVelocity(entry->body, { 0.0f, 0.0f, 0.0f });
        b3Body_SetAwake(entry->body, true);
        entry->current = { b3ToPos(ToB3(position)), rotation };
        entry->previous = entry->current;
    }
}

void CollisionWorld::SetTargetPosition(ColliderHandle handle, const Vector3& position, float timeStep)
{
    Impl::Entry* entry = impl->Find(handle);
    if (!entry || timeStep <= 0.0f)
        return;

    b3WorldTransform target = b3Body_GetTransform(entry->body);
    target.p = b3ToPos(ToB3(position));
    b3Body_SetTargetTransform(entry->body, target, timeStep, true);
}

void CollisionWorld::SetTargetPose(ColliderHandle handle, const Vector3& parentPosition, const Vector3& parentRotation,
                                   const Vector3& localPosition, const float localRotation[4], float timeStep)
{
    Impl::Entry* entry = impl->Find(handle);
    if (!entry)
        return;

    const b3Quat parent = QuatFromEuler(parentRotation);
    const b3Quat local = b3NormalizeQuat({ { localRotation[0], localRotation[1], localRotation[2] }, localRotation[3] });
    const b3Vec3 position = b3Add(ToB3(parentPosition), b3RotateVector(parent, ToB3(localPosition)));
    const b3Quat rotation = b3NormalizeQuat(b3MulQuat(parent, local));
    if (entry->motion == ColliderMotion::Kinematic && timeStep > 0.0f)
    {
        const b3WorldTransform target = { b3ToPos(position), rotation };
        b3Body_SetTargetTransform(entry->body, target, timeStep, true);
    }
    else
    {
        b3Body_SetTransform(entry->body, b3ToPos(position), rotation);
    }
}

Vector3 CollisionWorld::GetWorldPosition(ColliderHandle handle) const
{
    const Impl::Entry* entry = impl->Find(handle);
    if (!entry)
        return {};
    return FromB3(b3ToVec3(b3Body_GetPosition(entry->body))) + entry->offset;
}

void CollisionWorld::Step(float timeStep)
{
    KES_ZONE_FN(PHYSICS);
    if (timeStep <= 0.0f)
        return;

    b3World_Step(impl->world, timeStep, kSubStepCount);
    for (ColliderHandle handle : impl->dynamicBodies)
    {
        Impl::Entry& entry = impl->entries[handle];
        const b3WorldTransform transform = b3Body_GetTransform(entry.body);
        entry.previous = entry.current;
        entry.current = { transform.p, transform.q };
    }
}

bool CollisionWorld::GetBodyPose(ColliderHandle handle, float alpha, Vector3& outPosition, Vector3& outRotation) const
{
    const Impl::Entry* entry = impl->Find(handle);
    if (!entry)
        return false;

    const float t = ClampFloat(alpha, 0.0f, 1.0f);
    const Vector3 from = FromB3(b3ToVec3(entry->previous.position));
    const Vector3 to = FromB3(b3ToVec3(entry->current.position));
    outPosition = from + (to - from) * t;
    outRotation = EulerFromQuat(BlendQuat(entry->previous.rotation, entry->current.rotation, t));
    return true;
}

void CollisionWorld::ApplyImpulse(ColliderHandle handle, const Vector3& impulse)
{
    if (Impl::Entry* entry = impl->Find(handle))
        b3Body_ApplyLinearImpulseToCenter(entry->body, ToB3(impulse), true);
}

void CollisionWorld::SetLinearVelocity(ColliderHandle handle, const Vector3& velocity)
{
    if (Impl::Entry* entry = impl->Find(handle))
    {
        b3Body_SetLinearVelocity(entry->body, ToB3(velocity));
        b3Body_SetAwake(entry->body, true);
    }
}

Vector3 CollisionWorld::GetLinearVelocity(ColliderHandle handle) const
{
    const Impl::Entry* entry = impl->Find(handle);
    return entry ? FromB3(b3Body_GetLinearVelocity(entry->body)) : Vector3{};
}

bool CollisionWorld::CheckBox(const Vector3& position, const Vector3& halfExtents, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    b3Vec3 corners[8];
    int count = 0;
    for (float x : { -halfExtents.x, halfExtents.x })
        for (float y : { -halfExtents.y, halfExtents.y })
            for (float z : { -halfExtents.z, halfExtents.z })
                corners[count++] = { x, y, z };
    const b3ShapeProxy proxy = { corners, 8, 0.0f };
    return impl->Overlaps(position, proxy, ignore);
}

bool CollisionWorld::CheckSphere(const Vector3& position, float radius, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    const b3Vec3 center = { 0.0f, 0.0f, 0.0f };
    const b3ShapeProxy proxy = { &center, 1, radius };
    return impl->Overlaps(position, proxy, ignore);
}

bool CollisionWorld::CheckCapsule(const Vector3& position, float radius, float halfHeight, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    const CapsuleProxy capsule(radius, halfHeight);
    return impl->Overlaps(position, capsule.proxy, ignore);
}

CollisionHit CollisionWorld::SweepCapsule(const Vector3& from, const Vector3& to, float radius, float halfHeight, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    const CapsuleProxy capsule(radius, halfHeight);
    ClosestCastContext context;
    context.ignore = ignore;
    b3World_CastShape(impl->world, b3ToPos(ToB3(from)), &capsule.proxy, ToB3(to - from), MakeQueryFilter(false), ClosestCastCallback, &context);

    CollisionHit hit;
    if (context.found)
    {
        hit.hit = true;
        hit.other = context.other;
        hit.normal = context.normal;
        hit.fraction = context.fraction;
    }
    return hit;
}

bool CollisionWorld::Raycast(const Ray& ray, RaycastHit& outHit, const RaycastQuery& query) const
{
    KES_ZONE_FN(PHYSICS);
    outHit = {};

    const Vector3 direction = Vector3Normalize(ray.direction);
    if (Vector3LengthSq(direction) <= 0.00001f || query.maxDistance < 0.0f)
        return false;

    const b3QueryFilter filter = MakeQueryFilter(query.includeTriggers);
    const b3Pos origin = b3ToPos(ToB3(ray.origin));

    // Box3D ray casts ignore shapes the ray starts inside.
    const b3Vec3 point = { 0.0f, 0.0f, 0.0f };
    const b3ShapeProxy pointProxy = { &point, 1, 0.0f };
    OverlapContext inside;
    inside.ignore = query.ignore;
    b3World_OverlapShape(impl->world, origin, &pointProxy, filter, OverlapCallback, &inside);
    if (inside.found)
    {
        outHit.hit = true;
        outHit.other = inside.first;
        outHit.type = impl->Find(inside.first)->type;
        outHit.point = ray.origin;
        outHit.normal = direction * -1.0f;
        return true;
    }

    ClosestCastContext context;
    context.ignore = query.ignore;
    b3World_CastRay(impl->world, origin, ToB3(direction * query.maxDistance), filter, ClosestCastCallback, &context);
    if (!context.found)
        return false;

    outHit.hit = true;
    outHit.other = context.other;
    outHit.type = impl->Find(context.other)->type;
    outHit.point = context.point;
    outHit.normal = context.normal;
    outHit.distance = context.fraction * query.maxDistance;
    return true;
}

std::size_t CollisionWorld::RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits, const RaycastQuery& query) const
{
    KES_ZONE_FN(PHYSICS);
    outHits.clear();

    const Vector3 direction = Vector3Normalize(ray.direction);
    if (Vector3LengthSq(direction) <= 0.00001f || query.maxDistance < 0.0f)
        return 0;

    struct Context
    {
        const CollisionWorld::Impl* impl;
        std::vector<RaycastHit>* hits;
        ColliderHandle ignore;
        float maxDistance;
        Vector3 origin;
        Vector3 direction;
    } context = { impl.get(), &outHits, query.ignore, query.maxDistance, ray.origin, direction };

    const b3QueryFilter filter = MakeQueryFilter(query.includeTriggers);
    const b3Pos origin = b3ToPos(ToB3(ray.origin));

    const auto castCallback = [](b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* userContext) -> float
    {
        auto* cast = static_cast<Context*>(userContext);
        const ColliderHandle handle = HandleOf(shape);
        if (handle == cast->ignore)
            return -1.0f;
        RaycastHit hit;
        hit.hit = true;
        hit.other = handle;
        hit.type = cast->impl->Find(handle)->type;
        hit.point = FromB3(point);
        hit.normal = FromB3(normal);
        hit.distance = fraction * cast->maxDistance;
        cast->hits->push_back(hit);
        return 1.0f;
    };
    b3World_CastRay(impl->world, origin, ToB3(direction * query.maxDistance), filter, castCallback, &context);

    // Shapes the ray starts inside, which the cast skips.
    const auto overlapCallback = [](b3ShapeId shape, void* userContext) -> bool
    {
        auto* cast = static_cast<Context*>(userContext);
        const ColliderHandle handle = HandleOf(shape);
        if (handle == cast->ignore)
            return true;
        RaycastHit hit;
        hit.hit = true;
        hit.other = handle;
        hit.type = cast->impl->Find(handle)->type;
        hit.point = cast->origin;
        hit.normal = cast->direction * -1.0f;
        cast->hits->push_back(hit);
        return true;
    };
    const b3Vec3 point = { 0.0f, 0.0f, 0.0f };
    const b3ShapeProxy pointProxy = { &point, 1, 0.0f };
    b3World_OverlapShape(impl->world, origin, &pointProxy, filter, overlapCallback, &context);

    std::stable_sort(outHits.begin(), outHits.end(), [](const RaycastHit& a, const RaycastHit& b) { return a.distance < b.distance; });

    // One hit per collider: bodies can have several shapes.
    std::vector<RaycastHit> unique;
    unique.reserve(outHits.size());
    for (const RaycastHit& hit : outHits)
    {
        const bool seen = std::any_of(unique.begin(), unique.end(), [&hit](const RaycastHit& other) { return other.other == hit.other; });
        if (!seen)
            unique.push_back(hit);
    }
    outHits.swap(unique);
    return outHits.size();
}

Vector3 CollisionWorld::MoveCapsule(const Vector3& position, const Vector3& delta, float radius, float halfHeight, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);

    struct Context
    {
        const CollisionWorld::Impl* impl;
        ColliderHandle ignore;
        b3CollisionPlane planes[kMaxMoverPlanes];
        int planeCount;
    } context = {};
    context.impl = impl.get();
    context.ignore = ignore;

    const auto planeCallback = [](b3ShapeId shape, const b3PlaneResult* planes, int planeCount, void* userContext) -> bool
    {
        auto* mover = static_cast<Context*>(userContext);
        const ColliderHandle handle = HandleOf(shape);
        if (handle == mover->ignore)
            return true;
        const bool dynamic = mover->impl->Find(handle)->motion == ColliderMotion::Dynamic;
        for (int i = 0; i < planeCount && mover->planeCount < kMaxMoverPlanes; ++i)
        {
            b3CollisionPlane& plane = mover->planes[mover->planeCount++];
            plane.plane = planes[i].plane;
            plane.pushLimit = dynamic ? kDynamicPushLimit : FLT_MAX;
            plane.push = 0.0f;
            plane.clipVelocity = !dynamic;
        }
        return true;
    };
    const auto castFilter = [](b3ShapeId shape, void* userContext) -> bool
    {
        return HandleOf(shape) != static_cast<Context*>(userContext)->ignore;
    };

    const b3Capsule capsule = { { 0.0f, -halfHeight, 0.0f }, { 0.0f, halfHeight, 0.0f }, radius };
    b3QueryFilter collideFilter = MakeQueryFilter(false);
    b3QueryFilter sweepFilter = collideFilter;
    sweepFilter.maskBits = kCategorySolid;   // dynamic bodies are pushed, not hit

    Vector3 current = position;
    const Vector3 target = position + delta;
    for (int iteration = 0; iteration < kMaxMoverIterations; ++iteration)
    {
        context.planeCount = 0;
        const b3Pos origin = b3ToPos(ToB3(current));
        b3World_CollideMover(impl->world, origin, &capsule, collideFilter, planeCallback, &context);

        const b3PlaneSolverResult solved = b3SolvePlanes(ToB3(target - current), context.planes, context.planeCount);
        const float fraction = b3World_CastMover(impl->world, origin, &capsule, solved.delta, sweepFilter, castFilter, &context);
        const Vector3 step = FromB3(solved.delta) * fraction;
        current += step;
        if (Vector3LengthSq(step) < 0.0001f * 0.0001f)
            break;
    }
    return current;
}

void CollisionWorld::Clear()
{
    b3DestroyWorld(impl->world);
    impl->entries.clear();
    impl->dynamicBodies.clear();
    impl->nextHandle = 1;
    impl->CreateWorld();
}

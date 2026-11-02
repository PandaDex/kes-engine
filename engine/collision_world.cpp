#include "collision_world.h"
#include <algorithm>
#include <cmath>
#include "profiler.h"

namespace
{
    struct RaycastCandidate
    {
        bool hit = false;
        float distance = 0.0f;
        Vector3 point = {};
        Vector3 normal = {};
    };

    static Vector3 AbsVec3(const Vector3& v)
    {
        return { std::fabs(v.x), std::fabs(v.y), std::fabs(v.z) };
    }

    static float ClampScalar(float v, float minV, float maxV)
    {
        if (v < minV) return minV;
        if (v > maxV) return maxV;
        return v;
    }

    static Vector3 ClosestPointOnAABB(const Vector3& point, const Vector3& center, const Vector3& halfExtents)
    {
        return {
            ClampScalar(point.x, center.x - halfExtents.x, center.x + halfExtents.x),
            ClampScalar(point.y, center.y - halfExtents.y, center.y + halfExtents.y),
            ClampScalar(point.z, center.z - halfExtents.z, center.z + halfExtents.z)
        };
    }

    static bool BoxVsBox(const Vector3& aPos, const Vector3& aHalf, const Vector3& bPos, const Vector3& bHalf)
    {
        const Vector3 d = AbsVec3(aPos - bPos);
        return d.x <= (aHalf.x + bHalf.x) && d.y <= (aHalf.y + bHalf.y) && d.z <= (aHalf.z + bHalf.z);
    }

    static bool SphereVsSphere(const Vector3& aPos, float aRadius, const Vector3& bPos, float bRadius)
    {
        const float radiusSum = aRadius + bRadius;
        return Vector3LengthSq(aPos - bPos) <= radiusSum * radiusSum;
    }

    static bool SphereVsBox(const Vector3& spherePos, float radius, const Vector3& boxPos, const Vector3& boxHalf)
    {
        const Vector3 closest = ClosestPointOnAABB(spherePos, boxPos, boxHalf);
        return Vector3LengthSq(spherePos - closest) <= radius * radius;
    }

    static bool CapsuleVsSphere(const Vector3& capsulePos, float capsuleRadius, float capsuleHalfHeight, const Vector3& spherePos, float sphereRadius)
    {
        const float top = capsulePos.y + capsuleHalfHeight;
        const float bottom = capsulePos.y - capsuleHalfHeight;
        const Vector3 closest = { capsulePos.x, ClampScalar(spherePos.y, bottom, top), capsulePos.z };
        return SphereVsSphere(closest, capsuleRadius, spherePos, sphereRadius);
    }

    static bool CapsuleVsBox(const Vector3& capsulePos, float capsuleRadius, float capsuleHalfHeight, const Vector3& boxPos, const Vector3& boxHalf)
    {
        const float top = capsulePos.y + capsuleHalfHeight;
        const float bottom = capsulePos.y - capsuleHalfHeight;
        const Vector3 closestOnSegmentToBox = {
            capsulePos.x,
            ClampScalar(boxPos.y, bottom, top),
            capsulePos.z
        };
        return SphereVsBox(closestOnSegmentToBox, capsuleRadius, boxPos, boxHalf);
    }

    static bool CapsuleVsCapsule(const Vector3& aPos, float aRadius, float aHalfHeight, const Vector3& bPos, float bRadius, float bHalfHeight)
    {
        const float aTop = aPos.y + aHalfHeight;
        const float aBottom = aPos.y - aHalfHeight;
        const float bTop = bPos.y + bHalfHeight;
        const float bBottom = bPos.y - bHalfHeight;

        const float closestAy = ClampScalar((bTop + bBottom) * 0.5f, aBottom, aTop);
        const float closestBy = ClampScalar(closestAy, bBottom, bTop);

        const Vector3 pa = { aPos.x, closestAy, aPos.z };
        const Vector3 pb = { bPos.x, closestBy, bPos.z };
        return SphereVsSphere(pa, aRadius, pb, bRadius);
    }

    static bool PointInBox(const Vector3& point, const Vector3& center, const Vector3& halfExtents)
    {
        const Vector3 local = point - center;
        return std::fabs(local.x) <= halfExtents.x &&
            std::fabs(local.y) <= halfExtents.y &&
            std::fabs(local.z) <= halfExtents.z;
    }

    static bool PointInCapsule(const Vector3& point, const Vector3& center, float radius, float halfHeight)
    {
        const float clampedY = ClampScalar(point.y, center.y - halfHeight, center.y + halfHeight);
        const Vector3 closest = { center.x, clampedY, center.z };
        return Vector3LengthSq(point - closest) <= radius * radius;
    }

    static Vector3 SafeNormal(const Vector3& value, const Vector3& fallback)
    {
        const float lengthSq = Vector3LengthSq(value);
        if (lengthSq <= 0.00001f)
            return fallback;
        return value / std::sqrt(lengthSq);
    }

    static bool RayVsSphere(const Ray& ray, const Vector3& center, float radius, float maxDistance, RaycastCandidate& outHit)
    {
        const Vector3 toOrigin = ray.origin - center;
        const float b = Vector3Dot(toOrigin, ray.direction);
        const float c = Vector3Dot(toOrigin, toOrigin) - radius * radius;

        if (c <= 0.0f)
        {
            outHit.hit = true;
            outHit.distance = 0.0f;
            outHit.point = ray.origin;
            outHit.normal = SafeNormal(ray.origin - center, ray.direction * -1.0f);
            return true;
        }

        const float discriminant = b * b - c;
        if (discriminant < 0.0f)
            return false;

        float distance = -b - std::sqrt(discriminant);
        if (distance < 0.0f)
            distance = -b + std::sqrt(discriminant);
        if (distance < 0.0f || distance > maxDistance)
            return false;

        outHit.hit = true;
        outHit.distance = distance;
        outHit.point = ray.origin + ray.direction * distance;
        outHit.normal = SafeNormal(outHit.point - center, ray.direction * -1.0f);
        return true;
    }

    static bool RayVsBox(const Ray& ray, const Vector3& center, const Vector3& halfExtents, float maxDistance, RaycastCandidate& outHit)
    {
        if (PointInBox(ray.origin, center, halfExtents))
        {
            outHit.hit = true;
            outHit.distance = 0.0f;
            outHit.point = ray.origin;
            outHit.normal = ray.direction * -1.0f;
            return true;
        }

        const Vector3 origin = ray.origin - center;
        float tMin = 0.0f;
        float tMax = maxDistance;
        int hitAxis = -1;
        float hitSign = 0.0f;

        const float originValues[3] = { origin.x, origin.y, origin.z };
        const float directionValues[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
        const float halfValues[3] = { halfExtents.x, halfExtents.y, halfExtents.z };

        for (int axis = 0; axis < 3; ++axis)
        {
            const float axisOrigin = originValues[axis];
            const float axisDirection = directionValues[axis];
            const float axisHalf = halfValues[axis];

            if (std::fabs(axisDirection) <= 0.00001f)
            {
                if (axisOrigin < -axisHalf || axisOrigin > axisHalf)
                    return false;
                continue;
            }

            const float invDirection = 1.0f / axisDirection;
            float t1 = (-axisHalf - axisOrigin) * invDirection;
            float t2 = (axisHalf - axisOrigin) * invDirection;
            float nearDistance = t1;
            float farDistance = t2;
            float nearSign = axisDirection > 0.0f ? -1.0f : 1.0f;

            if (nearDistance > farDistance)
            {
                const float temp = nearDistance;
                nearDistance = farDistance;
                farDistance = temp;
            }

            if (nearDistance > tMin)
            {
                tMin = nearDistance;
                hitAxis = axis;
                hitSign = nearSign;
            }

            if (farDistance < tMax)
                tMax = farDistance;

            if (tMin > tMax)
                return false;
        }

        if (hitAxis < 0 || tMin < 0.0f || tMin > maxDistance)
            return false;

        outHit.hit = true;
        outHit.distance = tMin;
        outHit.point = ray.origin + ray.direction * tMin;
        outHit.normal = {};
        if (hitAxis == 0) outHit.normal.x = hitSign;
        if (hitAxis == 1) outHit.normal.y = hitSign;
        if (hitAxis == 2) outHit.normal.z = hitSign;
        return true;
    }

    static bool RayVsVerticalCylinder(const Ray& ray, const Vector3& center, float radius, float halfHeight, float maxDistance, RaycastCandidate& outHit)
    {
        const Vector3 origin = ray.origin - center;
        const float a = ray.direction.x * ray.direction.x + ray.direction.z * ray.direction.z;
        if (a <= 0.00001f)
            return false;

        const float b = 2.0f * (origin.x * ray.direction.x + origin.z * ray.direction.z);
        const float c = origin.x * origin.x + origin.z * origin.z - radius * radius;
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant < 0.0f)
            return false;

        const float sqrtDiscriminant = std::sqrt(discriminant);
        const float invDenominator = 0.5f / a;
        const float candidates[2] = {
            (-b - sqrtDiscriminant) * invDenominator,
            (-b + sqrtDiscriminant) * invDenominator
        };

        for (float distance : candidates)
        {
            if (distance < 0.0f || distance > maxDistance)
                continue;

            const float y = origin.y + ray.direction.y * distance;
            if (y < -halfHeight || y > halfHeight)
                continue;

            const Vector3 localPoint = origin + ray.direction * distance;
            outHit.hit = true;
            outHit.distance = distance;
            outHit.point = ray.origin + ray.direction * distance;
            outHit.normal = SafeNormal({ localPoint.x, 0.0f, localPoint.z }, ray.direction * -1.0f);
            return true;
        }

        return false;
    }

    static bool RayVsCapsule(const Ray& ray, const Vector3& center, float radius, float halfHeight, float maxDistance, RaycastCandidate& outHit)
    {
        if (PointInCapsule(ray.origin, center, radius, halfHeight))
        {
            outHit.hit = true;
            outHit.distance = 0.0f;
            outHit.point = ray.origin;
            outHit.normal = ray.direction * -1.0f;
            return true;
        }

        RaycastCandidate bestHit = {};
        auto considerHit = [&](const RaycastCandidate& candidate)
        {
            if (!candidate.hit)
                return;
            if (!bestHit.hit || candidate.distance < bestHit.distance)
                bestHit = candidate;
        };

        RaycastCandidate sideHit = {};
        considerHit(RayVsVerticalCylinder(ray, center, radius, halfHeight, maxDistance, sideHit) ? sideHit : RaycastCandidate{});

        RaycastCandidate capHit = {};
        considerHit(RayVsSphere(ray, { center.x, center.y + halfHeight, center.z }, radius, maxDistance, capHit) ? capHit : RaycastCandidate{});

        capHit = {};
        considerHit(RayVsSphere(ray, { center.x, center.y - halfHeight, center.z }, radius, maxDistance, capHit) ? capHit : RaycastCandidate{});

        if (!bestHit.hit)
            return false;

        outHit = bestHit;
        return true;
    }

    // --- Convex hulls ------------------------------------------------------------
    // Separating-axis style tests using the hull face planes plus the world axes
    // (the hull AABB). Probe shapes are handled by pushing each plane out by the
    // probe support distance, which is exact on faces and slightly conservative
    // on edges/corners - the classic brush-collision trade-off.

    static bool AabbOverlap(const Vector3& aMin, const Vector3& aMax, const Vector3& bMin, const Vector3& bMax)
    {
        return aMin.x <= bMax.x && aMax.x >= bMin.x &&
               aMin.y <= bMax.y && aMax.y >= bMin.y &&
               aMin.z <= bMax.z && aMax.z >= bMin.z;
    }

    // support(n) = how far the probe reaches along n from its center.
    template<typename SupportFn>
    static bool ConvexVsProbe(const CollisionBody& hull, const Vector3& hullPos, const Vector3& center,
                              const Vector3& probeHalfExtents, SupportFn support)
    {
        const Vector3 hullMin = hullPos + hull.convex.boundsMin;
        const Vector3 hullMax = hullPos + hull.convex.boundsMax;
        if (!AabbOverlap(hullMin, hullMax, center - probeHalfExtents, center + probeHalfExtents))
            return false;

        const Vector3 local = center - hullPos;
        for (const CollisionPlane& plane : hull.convex.planes)
        {
            if (Vector3Dot(plane.normal, local) - plane.dist > support(plane.normal))
                return false;
        }
        return true;
    }

    static bool ConvexVsSphere(const CollisionBody& hull, const Vector3& hullPos, const Vector3& center, float radius)
    {
        return ConvexVsProbe(hull, hullPos, center, { radius, radius, radius },
                             [radius](const Vector3&) { return radius; });
    }

    static bool ConvexVsBox(const CollisionBody& hull, const Vector3& hullPos, const Vector3& center, const Vector3& half)
    {
        return ConvexVsProbe(hull, hullPos, center, half, [&half](const Vector3& n)
        {
            return std::fabs(n.x) * half.x + std::fabs(n.y) * half.y + std::fabs(n.z) * half.z;
        });
    }

    // Vertical capsule = segment along Y swept by a sphere.
    static bool ConvexVsCapsule(const CollisionBody& hull, const Vector3& hullPos, const Vector3& center, float radius, float halfHeight)
    {
        return ConvexVsProbe(hull, hullPos, center, { radius, halfHeight + radius, radius },
                             [radius, halfHeight](const Vector3& n) { return radius + std::fabs(n.y) * halfHeight; });
    }

    static bool PointInConvex(const CollisionBody& hull, const Vector3& hullPos, const Vector3& point)
    {
        const Vector3 local = point - hullPos;
        for (const CollisionPlane& plane : hull.convex.planes)
        {
            if (Vector3Dot(plane.normal, local) - plane.dist > 0.0f)
                return false;
        }
        return !hull.convex.planes.empty();
    }

    // Ray clipped against every half-space (Cyrus-Beck).
    static bool RayVsConvex(const Ray& ray, const CollisionBody& hull, const Vector3& hullPos, float maxDistance, RaycastCandidate& outHit)
    {
        if (hull.convex.planes.empty())
            return false;

        if (PointInConvex(hull, hullPos, ray.origin))
        {
            outHit.hit = true;
            outHit.distance = 0.0f;
            outHit.point = ray.origin;
            outHit.normal = ray.direction * -1.0f;
            return true;
        }

        const Vector3 origin = ray.origin - hullPos;
        float tEnter = 0.0f;
        float tExit = maxDistance;
        Vector3 enterNormal = {};
        bool entered = false;

        for (const CollisionPlane& plane : hull.convex.planes)
        {
            const float distance = Vector3Dot(plane.normal, origin) - plane.dist;
            const float rate = Vector3Dot(plane.normal, ray.direction);
            if (std::fabs(rate) <= 0.000001f)
            {
                if (distance > 0.0f)
                    return false;   // parallel and outside
                continue;
            }
            const float t = -distance / rate;
            if (rate < 0.0f)
            {
                if (t > tEnter)
                {
                    tEnter = t;
                    enterNormal = plane.normal;
                    entered = true;
                }
            }
            else if (t < tExit)
            {
                tExit = t;
            }
            if (tEnter > tExit)
                return false;
        }

        if (!entered || tEnter > maxDistance)
            return false;

        outHit.hit = true;
        outHit.distance = tEnter;
        outHit.point = ray.origin + ray.direction * tEnter;
        outHit.normal = enterNormal;
        return true;
    }

    static bool RaycastBody(const CollisionBody& body, const Ray& ray, float maxDistance, RaycastHit& outHit)
    {
        RaycastCandidate candidate = {};
        const Vector3 bodyPosition = body.position + body.offset;

        if (body.type == ColliderType::Box)
        {
            if (!RayVsBox(ray, bodyPosition, body.box.halfExtents, maxDistance, candidate))
                return false;
        }
        else if (body.type == ColliderType::Sphere)
        {
            if (!RayVsSphere(ray, bodyPosition, body.sphere.radius, maxDistance, candidate))
                return false;
        }
        else if (body.type == ColliderType::Capsule)
        {
            if (!RayVsCapsule(ray, bodyPosition, body.capsule.radius, body.capsule.halfHeight, maxDistance, candidate))
                return false;
        }
        else if (body.type == ColliderType::Convex)
        {
            if (!RayVsConvex(ray, body, bodyPosition, maxDistance, candidate))
                return false;
        }
        else
        {
            return false;
        }

        outHit.hit = true;
        outHit.other = body.handle;
        outHit.type = body.type;
        outHit.point = candidate.point;
        outHit.normal = SafeNormal(candidate.normal, ray.direction * -1.0f);
        outHit.distance = candidate.distance;
        return true;
    }
}

ColliderHandle CollisionWorld::AddBox(const Vector3& position, const Vector3& halfExtents, bool isTrigger, const Vector3& offset)
{
    CollisionBody body = {};
    body.handle = nextHandle++;
    body.type = ColliderType::Box;
    body.position = position;
    body.offset = offset;
    body.box.halfExtents = halfExtents;
    body.isTrigger = isTrigger;
    bodies.push_back(body);
    return body.handle;
}

ColliderHandle CollisionWorld::AddSphere(const Vector3& position, float radius, bool isTrigger, const Vector3& offset)
{
    CollisionBody body = {};
    body.handle = nextHandle++;
    body.type = ColliderType::Sphere;
    body.position = position;
    body.offset = offset;
    body.sphere.radius = radius;
    body.isTrigger = isTrigger;
    bodies.push_back(body);
    return body.handle;
}

ColliderHandle CollisionWorld::AddCapsule(const Vector3& position, float radius, float halfHeight, bool isTrigger, const Vector3& offset)
{
    CollisionBody body = {};
    body.handle = nextHandle++;
    body.type = ColliderType::Capsule;
    body.position = position;
    body.offset = offset;
    body.capsule.radius = radius;
    body.capsule.halfHeight = halfHeight;
    body.isTrigger = isTrigger;
    bodies.push_back(body);
    return body.handle;
}

ColliderHandle CollisionWorld::AddConvex(const Vector3& position, const std::vector<CollisionPlane>& planes,
                                         const Vector3& boundsMin, const Vector3& boundsMax, bool isTrigger)
{
    CollisionBody body = {};
    body.handle = nextHandle++;
    body.type = ColliderType::Convex;
    body.position = position;
    body.convex.planes = planes;
    body.convex.boundsMin = boundsMin;
    body.convex.boundsMax = boundsMax;
    body.isTrigger = isTrigger;
    bodies.push_back(std::move(body));
    return bodies.back().handle;
}

bool CollisionWorld::Remove(ColliderHandle handle)
{
    for (std::size_t i = 0; i < bodies.size(); ++i)
    {
        if (bodies[i].handle == handle)
        {
            bodies.erase(bodies.begin() + (long long)i);
            return true;
        }
    }
    return false;
}

CollisionBody* CollisionWorld::Find(ColliderHandle handle)
{
    for (CollisionBody& body : bodies)
    {
        if (body.handle == handle)
            return &body;
    }
    return nullptr;
}

const CollisionBody* CollisionWorld::Find(ColliderHandle handle) const
{
    for (const CollisionBody& body : bodies)
    {
        if (body.handle == handle)
            return &body;
    }
    return nullptr;
}

void CollisionWorld::SetPosition(ColliderHandle handle, const Vector3& position)
{
    CollisionBody* body = Find(handle);
    if (body)
        body->position = position;
}

Vector3 CollisionWorld::GetWorldPosition(ColliderHandle handle) const
{
    const CollisionBody* body = Find(handle);
    if (!body)
        return {};
    return body->position + body->offset;
}

bool CollisionWorld::CheckBox(const Vector3& position, const Vector3& halfExtents, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    CollisionBody probe = {};
    probe.type = ColliderType::Box;
    probe.position = position;
    probe.box.halfExtents = halfExtents;
    return OverlapsBody(probe, ignore);
}

bool CollisionWorld::CheckSphere(const Vector3& position, float radius, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    CollisionBody probe = {};
    probe.type = ColliderType::Sphere;
    probe.position = position;
    probe.sphere.radius = radius;
    return OverlapsBody(probe, ignore);
}

bool CollisionWorld::CheckCapsule(const Vector3& position, float radius, float halfHeight, ColliderHandle ignore) const
{
    KES_ZONE_FN(PHYSICS);
    CollisionBody probe = {};
    probe.type = ColliderType::Capsule;
    probe.position = position;
    probe.capsule.radius = radius;
    probe.capsule.halfHeight = halfHeight;
    return OverlapsBody(probe, ignore);
}

CollisionHit CollisionWorld::SweepCapsule(const Vector3& from, const Vector3& to, float radius, float halfHeight, ColliderHandle ignore, int steps) const
{
    KES_ZONE_FN(PHYSICS);
    CollisionHit hit = {};
    if (steps < 1)
        steps = 1;

    for (int i = 1; i <= steps; ++i)
    {
        const float t = (float)i / (float)steps;
        const Vector3 sample = from + (to - from) * t;
        CollisionBody probe = {};
        probe.type = ColliderType::Capsule;
        probe.position = sample;
        probe.capsule.radius = radius;
        probe.capsule.halfHeight = halfHeight;

        for (const CollisionBody& body : bodies)
        {
            if (!body.active || body.handle == ignore || body.isTrigger)
                continue;
            if (OverlapsPair(probe, body))
            {
                hit.hit = true;
                hit.other = body.handle;
                return hit;
            }
        }
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

    const Ray normalizedRay = { ray.origin, direction };

    RaycastHit bestHit = {};
    for (const CollisionBody& body : bodies)
    {
        if (!body.active || body.handle == query.ignore)
            continue;
        if (!query.includeTriggers && body.isTrigger)
            continue;

        RaycastHit candidate = {};
        if (!RaycastBody(body, normalizedRay, query.maxDistance, candidate))
            continue;

        if (!bestHit.hit || candidate.distance < bestHit.distance)
            bestHit = candidate;
    }

    if (!bestHit.hit)
        return false;

    outHit = bestHit;
    return true;
}

std::size_t CollisionWorld::RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits, const RaycastQuery& query) const
{
    KES_ZONE_FN(PHYSICS);
    outHits.clear();

    const Vector3 direction = Vector3Normalize(ray.direction);
    if (Vector3LengthSq(direction) <= 0.00001f || query.maxDistance < 0.0f)
        return 0;

    const Ray normalizedRay = { ray.origin, direction };

    for (const CollisionBody& body : bodies)
    {
        if (!body.active || body.handle == query.ignore)
            continue;
        if (!query.includeTriggers && body.isTrigger)
            continue;

        RaycastHit hit = {};
        if (RaycastBody(body, normalizedRay, query.maxDistance, hit))
            outHits.push_back(hit);
    }

    std::sort(outHits.begin(), outHits.end(),
        [](const RaycastHit& a, const RaycastHit& b)
        {
            return a.distance < b.distance;
        });

    return outHits.size();
}

void CollisionWorld::Clear()
{
    bodies.clear();
    nextHandle = 1;
}

bool CollisionWorld::OverlapsBody(const CollisionBody& probe, ColliderHandle ignore) const
{
    for (const CollisionBody& body : bodies)
    {
        if (!body.active || body.handle == ignore)
            continue;
        if (OverlapsPair(probe, body))
            return true;
    }
    return false;
}

bool CollisionWorld::OverlapsPair(const CollisionBody& a, const CollisionBody& b) const
{
    const Vector3 aPos = a.position + a.offset;
    const Vector3 bPos = b.position + b.offset;

    if (a.type == ColliderType::Box && b.type == ColliderType::Box)
        return BoxVsBox(aPos, a.box.halfExtents, bPos, b.box.halfExtents);

    if (a.type == ColliderType::Sphere && b.type == ColliderType::Sphere)
        return SphereVsSphere(aPos, a.sphere.radius, bPos, b.sphere.radius);

    if (a.type == ColliderType::Sphere && b.type == ColliderType::Box)
        return SphereVsBox(aPos, a.sphere.radius, bPos, b.box.halfExtents);

    if (a.type == ColliderType::Box && b.type == ColliderType::Sphere)
        return SphereVsBox(bPos, b.sphere.radius, aPos, a.box.halfExtents);

    if (a.type == ColliderType::Capsule && b.type == ColliderType::Sphere)
        return CapsuleVsSphere(aPos, a.capsule.radius, a.capsule.halfHeight, bPos, b.sphere.radius);

    if (a.type == ColliderType::Sphere && b.type == ColliderType::Capsule)
        return CapsuleVsSphere(bPos, b.capsule.radius, b.capsule.halfHeight, aPos, a.sphere.radius);

    if (a.type == ColliderType::Capsule && b.type == ColliderType::Box)
        return CapsuleVsBox(aPos, a.capsule.radius, a.capsule.halfHeight, bPos, b.box.halfExtents);

    if (a.type == ColliderType::Box && b.type == ColliderType::Capsule)
        return CapsuleVsBox(bPos, b.capsule.radius, b.capsule.halfHeight, aPos, a.box.halfExtents);

    if (a.type == ColliderType::Capsule && b.type == ColliderType::Capsule)
        return CapsuleVsCapsule(aPos, a.capsule.radius, a.capsule.halfHeight, bPos, b.capsule.radius, b.capsule.halfHeight);

    // Convex hulls (map brushes) against the primitive shapes.
    if (a.type == ColliderType::Convex || b.type == ColliderType::Convex)
    {
        if (a.type == ColliderType::Convex && b.type == ColliderType::Convex)
            return false;   // brush vs brush: both static, never needed
        const bool aIsHull = a.type == ColliderType::Convex;
        const CollisionBody& hull = aIsHull ? a : b;
        const CollisionBody& other = aIsHull ? b : a;
        const Vector3 hullPos = aIsHull ? aPos : bPos;
        const Vector3 otherPos = aIsHull ? bPos : aPos;
        switch (other.type)
        {
        case ColliderType::Box:     return ConvexVsBox(hull, hullPos, otherPos, other.box.halfExtents);
        case ColliderType::Sphere:  return ConvexVsSphere(hull, hullPos, otherPos, other.sphere.radius);
        case ColliderType::Capsule: return ConvexVsCapsule(hull, hullPos, otherPos, other.capsule.radius, other.capsule.halfHeight);
        default:                    return false;
        }
    }

    return false;
}

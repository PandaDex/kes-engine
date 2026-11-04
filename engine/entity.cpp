#include "entity.h"
#include "collision_world.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include "profiler.h"

void Entity::Teleport(const Vector3& newPosition)
{
    Position = newPosition;
}

bool Entity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "UseGravity" || name == "GravityEnabled" || name == "Gravity")
    {
        gravityEnabled = entityprops::ParseBool(value, gravityEnabled);
        if (!gravityEnabled)
            verticalVelocity = 0.0f;
        return true;
    }

    return false;
}

bool Entity::ApplyRigidBodyProperty(RigidBodyDesc& desc, const std::string& name, const std::string& value)
{
    if (name == "Dynamic")
    {
        const bool dynamic = entityprops::ParseBool(value, desc.motion == ColliderMotion::Dynamic);
        desc.motion = dynamic ? ColliderMotion::Dynamic : ColliderMotion::Static;
        return true;
    }
    if (name == "Density")
    {
        desc.density = entityprops::ParseFloat(value, desc.density);
        return true;
    }
    if (name == "Friction")
    {
        desc.friction = entityprops::ParseFloat(value, desc.friction);
        return true;
    }
    if (name == "Restitution")
    {
        desc.restitution = entityprops::ParseFloat(value, desc.restitution);
        return true;
    }
    if (name == "RollingResistance")
    {
        desc.rollingResistance = entityprops::ParseFloat(value, desc.rollingResistance);
        return true;
    }
    return false;
}

void Entity::RunBuiltinPhysics(float fixedDt)
{
    KES_ZONE(PHYSICS, "Entity::RunBuiltinPhysics");
    if (!gravityEnabled || fixedDt <= 0.0f)
        return;

    verticalVelocity -= gravityAcceleration * fixedDt;

    Vector3 nextPosition = Position;
    nextPosition.y += verticalVelocity * fixedDt;

    if (game && game->collision && nextPosition.y <= Position.y)
    {
        RaycastQuery query = {};
        query.maxDistance = (Position.y - nextPosition.y) + 0.05f;
        query.includeTriggers = false;

        const Ray ray = { Position, { 0.0f, -1.0f, 0.0f } };
        std::vector<RaycastHit> hits;
        game->collision->RaycastAll(ray, hits, query);
        for (const RaycastHit& hit : hits)
        {
            if (OwnsCollider(hit.other))
                continue;
            if (hit.normal.y > 0.25f)
            {
                nextPosition.y = hit.point.y;
                verticalVelocity = 0.0f;
            }
            break;
        }
    }

    Teleport(nextPosition);
}

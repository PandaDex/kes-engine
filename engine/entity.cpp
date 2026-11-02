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
        RaycastHit hit = {};
        RaycastQuery query = {};
        query.maxDistance = (Position.y - nextPosition.y) + 0.05f;
        query.includeTriggers = false;

        const Ray ray = { Position, { 0.0f, -1.0f, 0.0f } };
        if (game->collision->Raycast(ray, hit, query) && hit.normal.y > 0.25f)
        {
            nextPosition.y = hit.point.y;
            verticalVelocity = 0.0f;
        }
    }

    Teleport(nextPosition);
}

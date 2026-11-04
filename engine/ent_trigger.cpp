#include "ent_trigger.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include <cmath>
#include <unordered_map>

void TriggerEntity::Update(float dt)
{
    (void)dt;
    if (!game || !game->entities)
        return;

    std::unordered_set<uint64_t> current;
    std::unordered_map<uint64_t, Entity*> activeEntities;
    game->entities->ForEachActive([&](Entity& entity)
    {
        if (&entity == this)
            return;

        activeEntities[entity.GetEntityId()] = &entity;
        if (IsInside(entity.Position))
            current.insert(entity.GetEntityId());
    });

    for (uint64_t id : current)
    {
        if (overlappingEntities.find(id) == overlappingEntities.end())
            game->ExecuteCommand(onEntityEnter, this, activeEntities[id]);
    }

    for (uint64_t id : overlappingEntities)
    {
        if (current.find(id) == current.end())
        {
            Entity* other = nullptr;
            auto activeIt = activeEntities.find(id);
            if (activeIt != activeEntities.end())
                other = activeIt->second;
            game->ExecuteCommand(onEntityExit, this, other);
        }
    }

    overlappingEntities = std::move(current);
}

bool TriggerEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "OnEntityEnter")
    {
        onEntityEnter = entityprops::ParseString(value);
        return true;
    }

    if (name == "OnEntityExit")
    {
        onEntityExit = entityprops::ParseString(value);
        return true;
    }

    if (name == "Size" || name == "Scale")
    {
        size = entityprops::ParseVector3(value, size);
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

bool TriggerEntity::IsInside(const Vector3& point) const
{
    const Vector3 half = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    const Vector3 delta = point - Position;
    return std::fabs(delta.x) <= half.x &&
        std::fabs(delta.y) <= half.y &&
        std::fabs(delta.z) <= half.z;
}

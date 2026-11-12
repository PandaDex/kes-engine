#include "ent_trigger.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include <cmath>

void TriggerEntity::Update(float dt)
{
    (void)dt;
    if (!game || !game->entities || !enabled)
        return;

    currentOverlaps.clear();
    enteredEntities.clear();
    game->entities->ForEachActive([&](Entity& entity)
    {
        if (&entity == this || !IsInside(entity.Position) || (!filterTag.empty() && !entity.HasTag(filterTag)))
            return;

        currentOverlaps.insert(entity.GetEntityId());
        if (overlappingEntities.find(entity.GetEntityId()) == overlappingEntities.end())
            enteredEntities.push_back(&entity);
    });

    for (Entity* entity : enteredEntities)
    {
        game->ExecuteCommand(onEntityEnter, this, entity);
        FireOutput("OnStartTouch", entity);
    }

    for (uint64_t id : overlappingEntities)
    {
        if (currentOverlaps.find(id) == currentOverlaps.end())
        {
            Entity* left = game->entities->GetEntityById(id);
            game->ExecuteCommand(onEntityExit, this, left);
            FireOutput("OnEndTouch", left);
        }
    }

    overlappingEntities.swap(currentOverlaps);
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

    if (name == "StartDisabled")
    {
        enabled = !entityprops::ParseBool(value, !enabled);
        return true;
    }

    if (name == "Filter")
    {
        filterTag = entityprops::ParseString(value);
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

const MapScriptPorts& TriggerEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = {
        { { "Enable" }, { "Disable" }, { "Toggle" } },
        { { "OnStartTouch" }, { "OnEndTouch" } },
    };
    return ports;
}

bool TriggerEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    (void)call;
    if (input != "Enable" && input != "Disable" && input != "Toggle")
        return false;
    enabled = input == "Enable" || (input == "Toggle" && !enabled);
    if (!enabled)
        overlappingEntities.clear();   // re-enabling fires OnStartTouch for whoever is inside
    return true;
}

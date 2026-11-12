#include "map_script.h"
#include <algorithm>
#include "entity.h"
#include "game_context.h"
#include "logger.h"
#include "profiler.h"

namespace
{
    bool HasPort(const std::vector<MapScriptPort>& ports, std::string_view name)
    {
        return std::any_of(ports.begin(), ports.end(), [&](const MapScriptPort& port) { return port.name == name; });
    }

    const char* NameOf(const Entity* entity)
    {
        return entity && !entity->Tags.empty() ? entity->Tags.front().c_str() : "<unnamed>";
    }
}

bool MapScriptPorts::HasInput(std::string_view name) const
{
    return name == "Kill" || HasPort(inputs, name);
}

bool MapScriptPorts::HasOutput(std::string_view name) const
{
    return HasPort(outputs, name);
}

void MapScriptSystem::Clear()
{
    links.clear();
    pending.clear();
    time = 0.0;
    nextOrder = 0;
    ++generation;
}

void MapScriptSystem::AddLink(const Guid& source, MapScriptLink link)
{
    if (!source.IsValid() || !link.target.IsValid() || link.output.empty() || link.input.empty())
        return;
    link.delay = std::max(link.delay, 0.0f);
    links[source].push_back(std::move(link));
}

size_t MapScriptSystem::GetLinkCount() const
{
    size_t count = 0;
    for (const auto& [source, list] : links)
        count += list.size();
    return count;
}

const std::vector<MapScriptLink>* MapScriptSystem::GetLinks(const Guid& source) const
{
    const auto it = links.find(source);
    return it == links.end() ? nullptr : &it->second;
}

void MapScriptSystem::FireOutput(Entity& source, std::string_view output, Entity* activator, std::string_view value)
{
    const auto it = links.find(source.GetGuid());
    if (it == links.end())
        return;

    std::vector<MapScriptLink>& list = it->second;
    for (MapScriptLink& link : list)
    {
        if (link.output != output)
            continue;
        Event event;
        event.dueTime = time + link.delay;
        event.order = nextOrder++;
        event.target = link.target;
        event.caller = source.GetGuid();
        event.activator = activator ? activator->GetGuid() : Guid{};
        event.input = link.input;
        event.value = link.parameter.empty() ? std::string(value) : link.parameter;
        pending.push_back(std::move(event));
    }
    list.erase(std::remove_if(list.begin(), list.end(), [&](const MapScriptLink& link)
                              { return link.once && link.output == output; }),
               list.end());
}

bool MapScriptSystem::SendInput(Entity& target, const std::string& input, Entity* caller, Entity* activator, const std::string& value)
{
    if (target.IsPendingDestroy())
        return false;
    IMapScriptable* scriptable = dynamic_cast<IMapScriptable*>(&target);
    if (!scriptable)
    {
        EngineLogf("MapScript: %s (%s) takes no inputs; \"%s\" ignored.",
                   NameOf(&target), target.GetGuid().ToString().c_str(), input.c_str());
        return false;
    }
    if (input == "Kill")
    {
        target.Destroy();
        return true;
    }
    const MapScriptCall call = { caller, activator, value };
    if (scriptable->AcceptInput(input, call))
        return true;
    EngineLogf("MapScript: %s (%s) does not handle input \"%s\".",
               NameOf(&target), target.GetGuid().ToString().c_str(), input.c_str());
    return false;
}

void MapScriptSystem::Update(float dt)
{
    KES_ZONE_FN(WORLD);
    time += dt;
    if (pending.empty())
        return;

    // Due events are taken out first: inputs may fire outputs, which append to `pending`.
    std::vector<Event> due;
    auto split = std::stable_partition(pending.begin(), pending.end(), [&](const Event& e) { return e.dueTime > time; });
    due.assign(std::make_move_iterator(split), std::make_move_iterator(pending.end()));
    pending.erase(split, pending.end());
    std::sort(due.begin(), due.end(), [](const Event& a, const Event& b)
              { return a.dueTime != b.dueTime ? a.dueTime < b.dueTime : a.order < b.order; });

    const uint64_t startGeneration = generation;
    dispatching = true;
    for (const Event& event : due)
    {
        if (generation != startGeneration)
            break;   // the level was cleared by an input
        Entity* target = Find(event.target);
        if (!target)
            continue;   // killed, or never spawned (unregistered class)
        SendInput(*target, event.input, Find(event.caller), Find(event.activator), event.value);
    }
    dispatching = false;
}

Entity* MapScriptSystem::Find(const Guid& guid) const
{
    if (!guid.IsValid() || !game || !game->entities)
        return nullptr;
    return game->entities->GetEntityByGuid(guid);
}

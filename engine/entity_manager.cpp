#include "entity_manager.h"
#include <algorithm>
#include <typeinfo>
#include "profiler.h"

EntityManager::~EntityManager()
{
    DestroyAll();
}

void EntityManager::StartPending()
{
    KES_ZONE_FN(WORLD);
    // Start() may create entities (spawners); those start in the same call.
    // The list is swapped out first because CreateEntity appends to it.
    for (int guard = 0; !pendingStart.empty() && guard < 64; ++guard)
    {
        std::vector<Entity*> starting;
        starting.swap(pendingStart);
        for (Entity* entity : starting)
        {
            if (entity && !entity->IsPendingDestroy())
            {
                KES_ZONE(ENTITIES, "Entity::Start");
                KES_ZONE_TEXTF(ENTITIES, "%s", typeid(*entity).name());
                entity->Start();
            }
        }
    }
}

// Indexed loops with the count taken up front: Update may create entities,
// which appends to `entities` (and may reallocate it). New entities run from
// the next frame, after their Start().
void EntityManager::Update(float dt)
{
    KES_ZONE_FN(WORLD);
    StartPending();

    const size_t count = entities.size();
    for (size_t i = 0; i < count; ++i)
    {
        Entity* entity = entities[i].get();
        if (!entity->IsPendingDestroy())
        {
            // Per-entity zone tagged with the C++ class name (KES_PROFILE_ENTITIES).
            KES_ZONE(ENTITIES, "Entity::Update");
            KES_ZONE_TEXTF(ENTITIES, "%s", typeid(*entity).name());
            entity->Update(dt);
        }
    }
}

void EntityManager::FixedUpdate(float fixedDt)
{
    KES_ZONE_FN(WORLD);
    StartPending();

    const size_t count = entities.size();
    for (size_t i = 0; i < count; ++i)
    {
        Entity* entity = entities[i].get();
        if (!entity->IsPendingDestroy())
        {
            KES_ZONE(ENTITIES, "Entity::FixedUpdate");
            KES_ZONE_TEXTF(ENTITIES, "%s", typeid(*entity).name());
            entity->FixedUpdate(fixedDt);
            entity->RunBuiltinPhysics(fixedDt);
        }
    }
}

void EntityManager::DestroyPending()
{
    KES_ZONE_FN(WORLD);
    pendingStart.erase(
        std::remove_if(
            pendingStart.begin(),
            pendingStart.end(),
            [](Entity* entity)
            {
                return entity == nullptr || entity->IsPendingDestroy();
            }),
        pendingStart.end());

    // OnDestroy may destroy or create other entities: indexed, and repeated
    // until a pass finds nothing new to destroy.
    for (bool ran = true; ran;)
    {
        ran = false;
        for (size_t i = 0; i < entities.size(); ++i)
        {
            Entity* entity = entities[i].get();
            if (entity->IsPendingDestroy() && !entity->destroyRan)
            {
                entity->destroyRan = true;
                entity->RunDestroy();
                ran = true;
            }
        }
    }

    entities.erase(
        std::remove_if(
            entities.begin(),
            entities.end(),
            [](const std::unique_ptr<Entity>& entity)
            {
                return entity->IsPendingDestroy();
            }),
        entities.end());
}

void EntityManager::DestroyAll()
{
    KES_ZONE_FN(WORLD);
    pendingStart.clear();

    for (size_t i = 0; i < entities.size(); ++i)
    {
        Entity* entity = entities[i].get();
        if (!entity->destroyRan)
        {
            entity->destroyRan = true;
            entity->RunDestroy();
        }
    }

    entities.clear();
    pendingStart.clear();   // entities created by OnDestroy are dropped too
    // Ids are not reset: they stay unique for the whole session, so an id kept
    // across a map change can never name a different entity.
}

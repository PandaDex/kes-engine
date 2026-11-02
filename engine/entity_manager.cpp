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
    for (Entity* entity : pendingStart)
    {
        if (entity && !entity->IsPendingDestroy())
        {
            KES_ZONE(ENTITIES, "Entity::Start");
            KES_ZONE_TEXTF(ENTITIES, "%s", typeid(*entity).name());
            entity->Start();
        }
    }
    pendingStart.clear();
}

void EntityManager::Update(float dt)
{
    KES_ZONE_FN(WORLD);
    StartPending();

    for (const std::unique_ptr<Entity>& entity : entities)
    {
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

    for (const std::unique_ptr<Entity>& entity : entities)
    {
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

    for (const std::unique_ptr<Entity>& entity : entities)
    {
        if (entity->IsPendingDestroy())
            entity->RunDestroy();
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

    for (const std::unique_ptr<Entity>& entity : entities)
    {
        entity->RunDestroy();
    }

    entities.clear();
    nextEntityId = 1;
}

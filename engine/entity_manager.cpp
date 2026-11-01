#include "entity_manager.h"
#include <algorithm>

EntityManager::~EntityManager()
{
    DestroyAll();
}

void EntityManager::StartPending()
{
    for (Entity* entity : pendingStart)
    {
        if (entity && !entity->IsPendingDestroy())
            entity->Start();
    }
    pendingStart.clear();
}

void EntityManager::Update(float dt)
{
    StartPending();

    for (const std::unique_ptr<Entity>& entity : entities)
    {
        if (!entity->IsPendingDestroy())
            entity->Update(dt);
    }
}

void EntityManager::FixedUpdate(float fixedDt)
{
    StartPending();

    for (const std::unique_ptr<Entity>& entity : entities)
    {
        if (!entity->IsPendingDestroy())
        {
            entity->FixedUpdate(fixedDt);
            entity->RunBuiltinPhysics(fixedDt);
        }
    }
}

void EntityManager::DestroyPending()
{
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
    pendingStart.clear();

    for (const std::unique_ptr<Entity>& entity : entities)
    {
        entity->RunDestroy();
    }

    entities.clear();
    nextEntityId = 1;
}

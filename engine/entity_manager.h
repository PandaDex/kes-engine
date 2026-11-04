#pragma once
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "entity.h"

struct GameContext;

class EntityManager
{
public:
    EntityManager() = default;
    ~EntityManager();

    void SetGameContext(GameContext* ctx) { game = ctx; }

    template<typename T, typename... Args>
    T* CreateEntity(Args&&... args)
    {
        static_assert(std::is_base_of_v<Entity, T>, "CreateEntity requires T to derive from Entity");
        std::unique_ptr<T> entity = std::make_unique<T>(std::forward<Args>(args)...);
        entity->SetGameContext(game);
        entity->entityId = nextEntityId++;
        T* ptr = entity.get();
        pendingStart.push_back(ptr);
        entities.push_back(std::move(entity));
        return ptr;
    }

    void StartPending();
    void Update(float dt);
    void FixedUpdate(float fixedDt);
    void DestroyPending();
    void DestroyAll();
    // True inside Update, FixedUpdate and DestroyPending, while the entity list
    // must not be cleared.
    bool IsUpdating() const { return updating; }

    template<typename T>
    T* GetEntityOfClass()
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;

            T* typed = dynamic_cast<T*>(entity.get());
            if (typed)
                return typed;
        }

        return nullptr;
    }

    template<typename T>
    const T* GetEntityOfClass() const
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;

            const T* typed = dynamic_cast<const T*>(entity.get());
            if (typed)
                return typed;
        }

        return nullptr;
    }

    template<typename T>
    T* GetEntityOfClassWithTag(const std::string& tag)
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;

            T* typed = dynamic_cast<T*>(entity.get());
            if (typed && typed->HasTag(tag))
                return typed;
        }

        return nullptr;
    }

    template<typename T>
    const T* GetEntityOfClassWithTag(const std::string& tag) const
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;

            const T* typed = dynamic_cast<const T*>(entity.get());
            if (typed && typed->HasTag(tag))
                return typed;
        }

        return nullptr;
    }

    Entity* GetEntityById(uint64_t entityId)
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;

            if (entity->GetEntityId() == entityId)
                return entity.get();
        }

        return nullptr;
    }

    const Entity* GetEntityById(uint64_t entityId) const
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;

            if (entity->GetEntityId() == entityId)
                return entity.get();
        }

        return nullptr;
    }

    template<typename Func>
    void ForEachActive(Func&& func)
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;
            func(*entity);
        }
    }

    template<typename Func>
    void ForEachActive(Func&& func) const
    {
        for (const std::unique_ptr<Entity>& entity : entities)
        {
            if (!entity || entity->IsPendingDestroy())
                continue;
            func(*entity);
        }
    }

private:
    std::vector<std::unique_ptr<Entity>> entities;
    std::vector<Entity*> pendingStart;
    GameContext* game = nullptr;
    uint64_t nextEntityId = 1;
    bool updating = false;
};

#pragma once
#include <string>
#include <vector>
#include "engine_types.h"

struct GameContext;
class EntityManager;

class Entity
{
public:
    virtual ~Entity() = default;

    Vector3 Position = {};
    Vector3 Rotation = {};
    std::vector<std::string> Tags;

    void SetGameContext(GameContext* ctx) { game = ctx; }
    uint64_t GetEntityId() const { return entityId; }

    virtual void Start() {}
    virtual void Update(float dt) { (void)dt; }
    virtual void FixedUpdate(float fixedDt) { (void)fixedDt; }
    virtual void Teleport(const Vector3& newPosition);

    virtual bool ApplyProperty(const std::string& name, const std::string& value);

    bool HasTag(const std::string& tag) const
    {
        for (const std::string& currentTag : Tags)
        {
            if (currentTag == tag)
                return true;
        }
        return false;
    }

    bool IsPendingDestroy() const { return pendingDestroy; }
    void Destroy() { pendingDestroy = true; }

protected:
    virtual void OnDestroy() {}

    GameContext* game = nullptr;

private:
    friend class EntityManager;

    void RunBuiltinPhysics(float fixedDt);
    void RunDestroy()
    {
        OnDestroy();
    }

    bool pendingDestroy = false;
    uint64_t entityId = 0;
    bool gravityEnabled = false;
    float verticalVelocity = 0.0f;
    float gravityAcceleration = 18.0f;
};

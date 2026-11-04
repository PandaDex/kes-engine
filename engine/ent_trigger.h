#pragma once
#include <string>
#include <unordered_set>
#include <vector>
#include "entity.h"

// Properties:
//   Size           volume extents, centered on the entity
//   OnEntityEnter  command run when a matching entity enters
//   OnEntityExit   command run when it leaves
//   Filter         tag an entity needs to count (default "Player"); empty = every entity
class TriggerEntity : public Entity
{
public:
    TriggerEntity() = default;
    ~TriggerEntity() override = default;

    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

    size_t GetOverlapCount() const { return overlappingEntities.size(); }

private:
    bool IsInside(const Vector3& point) const;

private:
    Vector3 size = { 1.0f, 1.0f, 1.0f };
    std::string onEntityEnter;
    std::string onEntityExit;
    std::string filterTag = kPlayerTag;
    std::unordered_set<uint64_t> overlappingEntities;
    std::unordered_set<uint64_t> currentOverlaps;   // scratch, kept to reuse its storage
    std::vector<Entity*> enteredEntities;           // scratch
};

#pragma once
#include <string>
#include <unordered_set>
#include "entity.h"

class TriggerEntity : public Entity
{
public:
    TriggerEntity() = default;
    ~TriggerEntity() override = default;

    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

private:
    bool IsInside(const Vector3& point) const;

private:
    Vector3 size = { 1.0f, 1.0f, 1.0f };
    std::string onEntityEnter;
    std::string onEntityExit;
    std::unordered_set<uint64_t> overlappingEntities;
};

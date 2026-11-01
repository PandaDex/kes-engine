#pragma once
#include "entity.h"
#include "input.h"
#include "collision_world.h"

class EnginePlayerEntity : public Entity
{
public:
    explicit EnginePlayerEntity(InputState& input);
    ~EnginePlayerEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    void FixedUpdate(float fixedDt) override;
    void Teleport(const Vector3& newPosition) override;

private:
    void SyncRuntimeCameraFromEntity() const;
    bool SyncEntityFromActiveCamera();
    void OnDestroy() override;

    InputState& input;
    uint64_t lastBoundCameraEntityId = 0;
    float moveSpeed = 4.0f;
    float mouseSensitivity = 0.0035f;
    float colliderRadius = 0.35f;
    float colliderHalfHeight = 0.9f;
    ColliderHandle collider = 0;
};

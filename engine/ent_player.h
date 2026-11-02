#pragma once
#include "entity.h"
#include "input.h"
#include "collision_world.h"

// Default player: a first-person fly camera with a collision capsule that
// slides along walls. Input actions (config/default.imap):
//   InputAction_Move       vector2  x = strafe, y = forward (analog: partial speed)
//   InputAction_MoveUp     boolean  rise
//   InputAction_MoveDown   boolean  sink
//   InputAction_Look       vector2  mouse deltas (an angle per frame)
//   InputAction_LookStick  vector2  gamepad stick (a turn rate)
// Look sensitivity and invert-Y come from GameSettings. Derive from this class
// (EngineRegisterPlayerClass) to build on it, or replace it entirely.
class EnginePlayerEntity : public Entity
{
public:
    explicit EnginePlayerEntity(InputState& input);
    ~EnginePlayerEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    void FixedUpdate(float fixedDt) override;
    void Teleport(const Vector3& newPosition) override;

    void SetMoveSpeed(float metersPerSecond) { moveSpeed = metersPerSecond; }
    float GetMoveSpeed() const { return moveSpeed; }
    // Base values; GameSettings sensitivities multiply them.
    void SetMouseLookScale(float radiansPerPixel) { mouseLookScale = radiansPerPixel; }
    void SetStickLookSpeed(float radiansPerSecond) { stickLookSpeed = radiansPerSecond; }
    // Disabled: no collision, the capsule passes through everything (noclip).
    void SetCollisionEnabled(bool enabled) { collisionEnabled = enabled; }
    bool IsCollisionEnabled() const { return collisionEnabled; }

protected:
    // Moves by `delta`, sliding along whatever blocks the capsule.
    void MoveWithCollision(const Vector3& delta);
    void UpdateLook(float dt);
    Vector3 GetForward() const;

    InputState& input;
    float moveSpeed = 4.0f;
    float mouseLookScale = 0.0035f;   // radians per mouse count
    float stickLookSpeed = 3.0f;      // radians per second at full tilt
    float colliderRadius = 0.35f;
    float colliderHalfHeight = 0.9f;
    bool collisionEnabled = true;
    ColliderHandle collider = 0;

private:
    void SyncRuntimeCameraFromEntity() const;
    bool SyncEntityFromActiveCamera();
    void OnDestroy() override;
    bool IsBlocked(const Vector3& from, const Vector3& to) const;

    uint64_t lastBoundCameraEntityId = 0;
};

#pragma once
#include "entity.h"
#include "input.h"
#include "collision_world.h"

enum class PlayerMovementMode
{
    Walking, // gravity, jumping, steps and slopes
    Flying, // free flight that still collides
    Noclip, // free flight through everything
};

// Default first-person player. Position is the center of its collision
// capsule; the camera sits EyeOffset above it. Input actions (config/default.imap):
//   InputAction_Move       vector2  x = strafe, y = forward (analog: partial speed)
//   InputAction_Jump       boolean  jump (walking)
//   InputAction_Sprint     boolean  run (walking)
//   InputAction_MoveUp     boolean  rise (flying)
//   InputAction_MoveDown   boolean  sink (flying)
//   InputAction_Look       vector2  mouse deltas (an angle per frame)
//   InputAction_LookStick  vector2  gamepad stick (a turn rate)
// Look sensitivity and invert-Y come from GameSettings. Debug builds: F3 cycles
// the movement modes. Derive from this class (EngineRegisterPlayerClass) to
// build on it, or replace it entirely.
class EnginePlayerEntity : public Entity
{
public:
    explicit EnginePlayerEntity(InputState& input);
    ~EnginePlayerEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    void FixedUpdate(float fixedDt) override;
    void Teleport(const Vector3& newPosition) override;
    bool OwnsCollider(ColliderHandle handle) const override;

    void SetMovementMode(PlayerMovementMode mode);
    PlayerMovementMode GetMovementMode() const { return movementMode; }
    bool IsGrounded() const { return grounded; }
    const Vector3& GetVelocity() const { return velocity; }
    Vector3 GetEyePosition() const;

    void SetMoveSpeed(float metersPerSecond) { moveSpeed = metersPerSecond; }
    float GetMoveSpeed() const { return moveSpeed; }
    void SetSprintSpeed(float metersPerSecond) { sprintSpeed = metersPerSecond; }
    void SetFlySpeed(float metersPerSecond) { flySpeed = metersPerSecond; }
    void SetJumpHeight(float meters) { jumpHeight = meters; }
    void SetGravity(float metersPerSecondSq) { gravity = metersPerSecondSq; }
    // Base values; GameSettings sensitivities multiply them.
    void SetMouseLookScale(float radiansPerPixel) { mouseLookScale = radiansPerPixel; }
    void SetStickLookSpeed(float radiansPerSecond) { stickLookSpeed = radiansPerSecond; }
    // Kept for older game code: false is Noclip, true restores Flying.
    void SetCollisionEnabled(bool enabled) { SetMovementMode(enabled ? PlayerMovementMode::Flying : PlayerMovementMode::Noclip); }
    bool IsCollisionEnabled() const { return movementMode != PlayerMovementMode::Noclip; }

protected:
    // Moves by `delta`, sliding along whatever blocks the capsule.
    void MoveWithCollision(const Vector3& delta);
    void UpdateLook(float dt);
    void UpdateFlying(float dt);
    void StepWalking(float dt);
    Vector3 GetForward() const;
    Vector3 GetWishDirection(bool includeVertical) const;

    InputState& input;
    PlayerMovementMode movementMode = PlayerMovementMode::Walking;
    float moveSpeed = 4.0f; // walking
    float sprintSpeed = 7.0f;
    float flySpeed = 6.0f;
    float groundAcceleration = 50.0f;
    float airAcceleration = 8.0f;
    float groundDeceleration = 40.0f; // m/s^2 without input
    float jumpHeight = 1.1f;
    float gravity = 20.0f;
    float stepHeight = 0.4f;
    float maxSlopeCos = 0.7f; // ~45 degrees
    float eyeOffset = 0.7f; // eye above the capsule center (1.6 m above the feet)
    float mouseLookScale = 0.0035f; // radians per mouse count
    float stickLookSpeed = 3.0f; // radians per second at full tilt
    float colliderRadius = 0.35f;
    float colliderHalfHeight = 0.55f; // capsule 1.8 m tall
    float fallRespawnDepth = 200.0f; // meters below the spawn point that count as fallen out of the map
    ColliderHandle collider = 0;

private:
    bool ProbeGround(const Vector3& center, float distance, float& outDrop) const;
    void SyncRuntimeCameraFromEntity(const Vector3& position) const;
    bool SyncEntityFromActiveCamera();
    void OnDestroy() override;

    Vector3 velocity = {};
    Vector3 previousPosition = {}; // last fixed step, for interpolation
    Vector3 spawnPosition = {};
    bool grounded = false;
    float jumpBuffer = 0.0f; // seconds a jump press stays queued
    float coyoteTime = 0.0f; // seconds a jump is still allowed after walking off a ledge
    uint64_t lastBoundCameraEntityId = 0;
};

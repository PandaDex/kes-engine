#include "ent_player.h"
#include <algorithm>
#include <cmath>
#include "ent_camera.h"
#include "game_context.h"
#include "game_settings.h"
#include "profiler.h"

namespace
{
    constexpr float kPi = 3.14159265358979f;
    constexpr float kMaxPitch = 1.4f;   // just under straight up / down

    void ApplyTransformToCamera(Camera& camera, const Vector3& position, const Vector3& rotation)
    {
        camera.x = position.x;
        camera.y = position.y;
        camera.z = position.z;
        camera.pitch = rotation.x;
        camera.yaw = rotation.y;
    }

    // Keeps yaw in (-pi, pi] so it never grows large enough to lose precision.
    float WrapAngle(float angle)
    {
        angle = std::fmod(angle + kPi, 2.0f * kPi);
        if (angle < 0.0f)
            angle += 2.0f * kPi;
        return angle - kPi;
    }
}

EnginePlayerEntity::EnginePlayerEntity(InputState& inputRef)
    : input(inputRef)
{
}

void EnginePlayerEntity::Start()
{
    SyncEntityFromActiveCamera();

    if (game && game->collision)
        collider = game->collision->AddCapsule(Position, colliderRadius, colliderHalfHeight, false, {}, ColliderMotion::Kinematic);
}

Vector3 EnginePlayerEntity::GetForward() const
{
    const float cp = std::cos(Rotation.x);
    const float sp = std::sin(Rotation.x);
    const float cy = std::cos(Rotation.y);
    const float sy = std::sin(Rotation.y);
    return Vector3Normalize({ sy * cp, -sp, cy * cp });
}

void EnginePlayerEntity::UpdateLook(float dt)
{
    const GameSettings* settings = GetGameSettings();
    const float mouseScale = mouseLookScale * (settings ? settings->GetMouseSensitivity() : 1.0f);
    const float stickScale = stickLookSpeed * (settings ? settings->GetGamepadLookSensitivity() : 1.0f);
    const float invert = settings && settings->IsLookYInverted() ? -1.0f : 1.0f;

    // Mouse deltas are already an angle for this frame; the stick is a rate.
    // Squaring the stick (keeping its sign) gives fine aim near the center.
    const Vector2 mouse = InputActionValue2D(input, "InputAction_Look");
    const Vector2 stick = InputActionValue2D(input, "InputAction_LookStick");
    const float stickX = stick.x * std::fabs(stick.x);
    const float stickY = stick.y * std::fabs(stick.y);

    Rotation.y = WrapAngle(Rotation.y + mouse.x * mouseScale + stickX * stickScale * dt);
    Rotation.x += (mouse.y * mouseScale + stickY * stickScale * dt) * invert;
    Rotation.x = ClampFloat(Rotation.x, -kMaxPitch, kMaxPitch);
}

void EnginePlayerEntity::Update(float dt)
{
    KES_ZONE(ENTITIES, "Player update");
    const CameraEntity* activeCamera = game ? game->GetActiveCameraEntity() : nullptr;
    const uint64_t activeCameraEntityId = activeCamera && activeCamera->IsPlayerControlled() ? activeCamera->GetEntityId() : 0;
    if (activeCameraEntityId != 0 && activeCameraEntityId != lastBoundCameraEntityId)
        SyncEntityFromActiveCamera();
    lastBoundCameraEntityId = activeCameraEntityId;

    UpdateLook(dt);

    // Movement is relative to yaw only, so looking down doesn't slow you down.
    const float cy = std::cos(Rotation.y);
    const float sy = std::sin(Rotation.y);
    const Vector3 moveInput = InputActionValue3D(input, "InputAction_Move");
    Vector3 moveDir = {};
    moveDir.x = cy * moveInput.x + sy * moveInput.y;
    moveDir.z = -sy * moveInput.x + cy * moveInput.y;
    moveDir.y = moveInput.z;
    if (InputActionDown(input, "InputAction_MoveUp"))
        moveDir.y += 1.0f;
    if (InputActionDown(input, "InputAction_MoveDown"))
        moveDir.y -= 1.0f;

    // Clamp instead of normalizing: a half-tilted stick moves at half speed,
    // diagonals on the keyboard aren't faster.
    const float length = Vector3Length(moveDir);
    if (length > 1.0f)
        moveDir = moveDir / length;
    if (length > 0.0001f && dt > 0.0f)
        MoveWithCollision(moveDir * (moveSpeed * dt));

    SyncRuntimeCameraFromEntity();
}

void EnginePlayerEntity::MoveWithCollision(const Vector3& delta)
{
    if (!collisionEnabled || !game || !game->collision)
    {
        Position += delta;
        return;
    }

    Position = game->collision->MoveCapsule(Position, delta, colliderRadius, colliderHalfHeight, collider);
}

void EnginePlayerEntity::FixedUpdate(float fixedDt)
{
    // Follows at a velocity, which is what lets the capsule push dynamic bodies.
    if (game && game->collision && collider != 0)
        game->collision->SetTargetPosition(collider, Position, fixedDt);
}

void EnginePlayerEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    SyncRuntimeCameraFromEntity();
    if (game && game->collision && collider != 0)
        game->collision->SetPosition(collider, Position);
}

void EnginePlayerEntity::SyncRuntimeCameraFromEntity() const
{
    CameraEntity* activeCamera = game ? game->GetActiveCameraEntity() : nullptr;
    if (activeCamera && activeCamera->IsPlayerControlled())
    {
        activeCamera->SetWorldPose(Position, Rotation);
        if (game->mainCamera)
            activeCamera->ApplyToCamera(*game->mainCamera);
        return;
    }

    if (!activeCamera && game && game->mainCamera)
        ApplyTransformToCamera(*game->mainCamera, Position, Rotation);
}

bool EnginePlayerEntity::SyncEntityFromActiveCamera()
{
    const CameraEntity* activeCamera = game ? game->GetActiveCameraEntity() : nullptr;
    if (activeCamera && activeCamera->IsPlayerControlled())
    {
        Position = activeCamera->Position;
        Rotation = activeCamera->Rotation;
        return true;
    }

    if (!activeCamera && game && game->mainCamera)
    {
        Position = { game->mainCamera->x, game->mainCamera->y, game->mainCamera->z };
        Rotation = { game->mainCamera->pitch, game->mainCamera->yaw, 0.0f };
        return true;
    }

    return false;
}

bool EnginePlayerEntity::OwnsCollider(ColliderHandle handle) const
{
    return handle == collider;
}

void EnginePlayerEntity::OnDestroy()
{
    if (game && game->collision && collider != 0)
    {
        game->collision->Remove(collider);
        collider = 0;
    }
}

#include "ent_player.h"
#include <SDL3/SDL.h>
#include <cmath>
#include "ent_camera.h"
#include "game_context.h"
#include "audio_system.h"

namespace
{
    static void ApplyTransformToCamera(Camera& camera, const Vector3& position, const Vector3& rotation)
    {
        camera.x = position.x;
        camera.y = position.y;
        camera.z = position.z;
        camera.pitch = rotation.x;
        camera.yaw = rotation.y;
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
    {
        collider = game->collision->AddCapsule(
            Position,
            colliderRadius,
            colliderHalfHeight,
            false);
    }
}

void EnginePlayerEntity::Update(float dt)
{
    const CameraEntity* activeCamera = game ? game->GetActiveCameraEntity() : nullptr;
    const uint64_t activeCameraEntityId = activeCamera && activeCamera->IsPlayerControlled() ? activeCamera->GetEntityId() : 0;
    if (activeCameraEntityId != 0 && activeCameraEntityId != lastBoundCameraEntityId)
        SyncEntityFromActiveCamera();
    lastBoundCameraEntityId = activeCameraEntityId;

    const Vector2 lookInput = InputActionValue2D(input, "InputAction_Look");

    Rotation.y += lookInput.x * mouseSensitivity;
    Rotation.x += lookInput.y * mouseSensitivity;
    Rotation.x = ClampFloat(Rotation.x, -1.4f, 1.4f);

    const float cp = std::cos(Rotation.x);
    const float sp = std::sin(Rotation.x);
    const float cy = std::cos(Rotation.y);
    const float sy = std::sin(Rotation.y);

    const Vector3 forward = Vector3Normalize({ sy * cp, -sp, cy * cp });
    const Vector3 worldUp = { 0.0f, 1.0f, 0.0f };

    const Vector3 moveInput = InputActionValue3D(input, "InputAction_Move");

    Vector3 moveDir = {};
    moveDir.x = cy * moveInput.x + sy * moveInput.y;
    moveDir.y = moveInput.z;
    moveDir.z = -sy * moveInput.x + cy * moveInput.y;

    if (InputKeyDown(input, SDL_SCANCODE_SPACE))
        moveDir.y += 1.0f;
    if (InputKeyDown(input, SDL_SCANCODE_LCTRL))
        moveDir.y -= 1.0f;

    if (Vector3LengthSq(moveDir) > 0.000001f)
    {
        moveDir = Vector3Normalize(moveDir);
        const Vector3 desired = Position + moveDir * (moveSpeed * dt);

        bool blocked = false;
        if (game && game->collision)
        {
            const CollisionHit hit = game->collision->SweepCapsule(
                Position,
                desired,
                colliderRadius,
                colliderHalfHeight,
                collider,
                6);
            blocked = hit.hit;
        }

        if (!blocked)
        {
            Position = desired;
        }
    }

    if (game && game->collision && collider != 0)
        game->collision->SetPosition(collider, Position);

    if (game && game->audio)
    {
        AudioListener listener = {};
        listener.position = Position;
        listener.forward = forward;
        listener.up = worldUp;
        game->audio->SetListener(listener);
    }

    SyncRuntimeCameraFromEntity();
}

void EnginePlayerEntity::FixedUpdate(float fixedDt)
{
    (void)fixedDt;
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

void EnginePlayerEntity::OnDestroy()
{
    if (game && game->collision && collider != 0)
    {
        game->collision->Remove(collider);
        collider = 0;
    }
}
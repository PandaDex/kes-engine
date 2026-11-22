#include "ent_player.h"
#include <algorithm>
#include <cmath>
#include "ent_camera.h"
#include "engine_configuration.h"
#include "game_context.h"
#include "game_settings.h"
#include "logger.h"
#include "on_screen_debug.h"
#include "profiler.h"

namespace
{
    constexpr float kPi = 3.14159265358979f;
    constexpr float kMaxPitch = 1.4f; // just under straight up / down
    constexpr float kJumpBufferTime = 0.15f;
    constexpr float kCoyoteTime = 0.1f;
    constexpr float kTerminalVelocity = 50.0f;

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

    float HorizontalLength(const Vector3& v)
    {
        return std::sqrt(v.x * v.x + v.z * v.z);
    }

    Vector3 MoveTowards(const Vector3& current, const Vector3& target, float maxDelta)
    {
        const Vector3 diff = target - current;
        const float length = Vector3Length(diff);
        if (length <= maxDelta || length < 0.00001f)
            return target;
        return current + diff * (maxDelta / length);
    }

    const char* MovementModeName(PlayerMovementMode mode)
    {
        switch (mode)
        {
        case PlayerMovementMode::Walking: return "walking";
        case PlayerMovementMode::Flying:  return "flying";
        case PlayerMovementMode::Noclip:  return "noclip";
        }
        return "?";
    }
}

EnginePlayerEntity::EnginePlayerEntity(InputState& inputRef)
    : input(inputRef)
{
}

void EnginePlayerEntity::Start()
{
    SyncEntityFromActiveCamera();
    previousPosition = Position;
    spawnPosition = Position;

    if (game && game->collision)
    {
        collider = game->collision->AddCapsule(Position, colliderRadius, colliderHalfHeight, false, {}, ColliderMotion::Kinematic);
        if (movementMode == PlayerMovementMode::Noclip)
            game->collision->SetActive(collider, false);
    }
}

void EnginePlayerEntity::SetMovementMode(PlayerMovementMode mode)
{
    if (mode == movementMode)
        return;
    movementMode = mode;
    velocity = {};
    grounded = false;
    jumpBuffer = 0.0f;
    coyoteTime = 0.0f;
    previousPosition = Position;
    if (game && game->collision && collider != 0)
        game->collision->SetActive(collider, mode != PlayerMovementMode::Noclip);
}

Vector3 EnginePlayerEntity::GetEyePosition() const
{
    return Position + Vector3{ 0.0f, eyeOffset, 0.0f };
}

Vector3 EnginePlayerEntity::GetForward() const
{
    const float cp = std::cos(Rotation.x);
    const float sp = std::sin(Rotation.x);
    const float cy = std::cos(Rotation.y);
    const float sy = std::sin(Rotation.y);
    return Vector3Normalize({ sy * cp, -sp, cy * cp });
}

// Relative to yaw only, so looking down doesn't slow you down. Clamped instead
// of normalized: a half-tilted stick moves at half speed, keyboard diagonals
// aren't faster.
Vector3 EnginePlayerEntity::GetWishDirection(bool includeVertical) const
{
    const float cy = std::cos(Rotation.y);
    const float sy = std::sin(Rotation.y);
    const Vector3 moveInput = InputActionValue3D(input, "InputAction_Move");
    Vector3 wish = {};
    wish.x = cy * moveInput.x + sy * moveInput.y;
    wish.z = -sy * moveInput.x + cy * moveInput.y;
    if (includeVertical)
    {
        wish.y = moveInput.z;
        if (InputActionDown(input, "InputAction_MoveUp"))
            wish.y += 1.0f;
        if (InputActionDown(input, "InputAction_MoveDown"))
            wish.y -= 1.0f;
    }
    const float length = Vector3Length(wish);
    return length > 1.0f ? wish / length : wish;
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
    {
        SyncEntityFromActiveCamera();
        previousPosition = Position;
    }
    lastBoundCameraEntityId = activeCameraEntityId;

#if ENGINE_ALLOW_DEBUG_MODE
    if (InputKeyPressed(input, SDL_SCANCODE_F3))
    {
        SetMovementMode(static_cast<PlayerMovementMode>((static_cast<int>(movementMode) + 1) % 3));
        EngineLogf("Player movement: %s.", MovementModeName(movementMode));
        AddOnScreenDebugMessagef(reinterpret_cast<intptr_t>(this), 2.0f, DebugMessageType::Info, "Player movement: %s (F3)",
            MovementModeName(movementMode));
    }
#endif

    UpdateLook(dt);

    Vector3 cameraPosition = Position;
    if (movementMode == PlayerMovementMode::Walking)
    {
        // Simulated in FixedUpdate; the press is kept until the next step.
        if (InputActionPressed(input, "InputAction_Jump"))
            jumpBuffer = kJumpBufferTime;

        if (Position.y < spawnPosition.y - fallRespawnDepth)
        {
            EngineLog("Player fell out of the map; back to the spawn point.");
            Teleport(spawnPosition);
            velocity = {};
        }

        const float alpha = game ? ClampFloat(game->fixedStepAlpha, 0.0f, 1.0f) : 1.0f;
        cameraPosition = previousPosition + (Position - previousPosition) * alpha;
    }
    else
    {
        UpdateFlying(dt);
        previousPosition = Position;
        cameraPosition = Position;
    }

    SyncRuntimeCameraFromEntity(cameraPosition + Vector3{ 0.0f, eyeOffset, 0.0f });
}

void EnginePlayerEntity::UpdateFlying(float dt)
{
    const Vector3 wish = GetWishDirection(true);
    if (Vector3LengthSq(wish) > 0.0000001f && dt > 0.0f)
        MoveWithCollision(wish * (flySpeed * dt));
}

void EnginePlayerEntity::MoveWithCollision(const Vector3& delta)
{
    if (movementMode == PlayerMovementMode::Noclip || !game || !game->collision)
    {
        Position += delta;
        return;
    }

    Position = game->collision->MoveCapsule(Position, delta, colliderRadius, colliderHalfHeight, collider);
}

// Sweeps the capsule down from `center`. True when it lands on walkable
// ground within `distance`; outDrop is how far down that ground is.
bool EnginePlayerEntity::ProbeGround(const Vector3& center, float distance, float& outDrop) const
{
    if (!game || !game->collision)
        return false;

    const float lift = 0.02f; // starts slightly up so resting contact still registers
    // A thinner capsule with the same bottom point: walls and step edges the
    // player is touching from the side don't count as ground.
    const float skin = 0.03f;
    const Vector3 from = center + Vector3{ 0.0f, lift, 0.0f };
    const Vector3 to = center - Vector3{ 0.0f, distance, 0.0f };
    const CollisionHit hit = game->collision->SweepCapsule(from, to, colliderRadius - skin, colliderHalfHeight + skin, collider);
    if (!hit.hit)
        return false;

    // Starting in contact can leave no normal; that's still the floor we stand on.
    const bool noNormal = Vector3LengthSq(hit.normal) < 0.01f;
    if (!noNormal && hit.normal.y < maxSlopeCos)
    {
        if (hit.normal.y <= 0.05f)
            return false; // a wall or ceiling
        // The rounded bottom on an edge (a step) gives a slanted normal. It
        // counts when the surface just past the contact is walkable.
        const float radius = colliderRadius - skin;
        const Vector3 hitCenter = from + (to - from) * hit.fraction;
        const Vector3 contact = hitCenter - Vector3{ 0.0f, colliderHalfHeight + skin, 0.0f } - hit.normal * radius;
        const Vector3 inward = Vector3Normalize(Vector3{ -hit.normal.x, 0.0f, -hit.normal.z });
        RaycastQuery query;
        query.maxDistance = 0.1f;
        query.ignore = collider;
        RaycastHit below;
        if (!game->collision->Raycast({ contact + inward * 0.02f + Vector3{ 0.0f, 0.05f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, below, query) ||
            below.distance <= 0.0f || below.normal.y < maxSlopeCos)
            return false;
    }

    outDrop = std::max(0.0f, hit.fraction * (distance + lift) - lift);
    return true;
}

void EnginePlayerEntity::StepWalking(float dt)
{
    if (!game || !game->collision || dt <= 0.0f)
        return;
    CollisionWorld& collision = *game->collision;

    const Vector3 wish = GetWishDirection(false);
    const bool hasInput = Vector3LengthSq(wish) > 0.0000001f;
    const float targetSpeed = InputActionDown(input, "InputAction_Sprint") ? sprintSpeed : moveSpeed;

    Vector3 horizontal = { velocity.x, 0.0f, velocity.z };
    if (grounded)
        horizontal = MoveTowards(horizontal, wish * targetSpeed, (hasInput ? groundAcceleration : groundDeceleration) * dt);
    else if (hasInput)
    {
        // Air control steers but never brakes below the speed you jumped with.
        const float speed = HorizontalLength(horizontal);
        horizontal = MoveTowards(horizontal, wish * std::max(targetSpeed, speed), airAcceleration * dt);
    }
    velocity.x = horizontal.x;
    velocity.z = horizontal.z;

    jumpBuffer = std::max(0.0f, jumpBuffer - dt);
    coyoteTime = std::max(0.0f, coyoteTime - dt);
    bool jumped = false;
    if (jumpBuffer > 0.0f && (grounded || coyoteTime > 0.0f))
    {
        velocity.y = std::sqrt(2.0f * gravity * jumpHeight);
        grounded = false;
        jumped = true;
        jumpBuffer = 0.0f;
        coyoteTime = 0.0f;
    }

    if (grounded)
        velocity.y = 0.0f;
    else
        velocity.y = std::max(velocity.y - gravity * dt, -kTerminalVelocity);

    const Vector3 start = Position;
    const Vector3 delta = velocity * dt;
    Vector3 moved = collision.MoveCapsule(start, delta, colliderRadius, colliderHalfHeight, collider);

    // Blocked while walking: try climbing a step (up, across, back down onto walkable ground).
    // The intended move, not the velocity: speed already lost against the step
    // mustn't keep the player from climbing it.
    Vector3 stepMove = { delta.x, 0.0f, delta.z };
    if (hasInput && HorizontalLength(stepMove) < targetSpeed * HorizontalLength(wish) * dt)
        stepMove = wish * (targetSpeed * dt);
    const float wanted = HorizontalLength(stepMove);
    const float achieved = HorizontalLength(moved - start);
    if (grounded && wanted > 0.0001f && achieved < wanted * 0.9f)
    {
        const Vector3 up = collision.MoveCapsule(start, { 0.0f, stepHeight, 0.0f }, colliderRadius, colliderHalfHeight, collider);
        const float raised = up.y - start.y;
        if (raised > 0.01f)
        {
            const Vector3 across = collision.MoveCapsule(up, stepMove, colliderRadius, colliderHalfHeight, collider);
            float drop = 0.0f;
            if (ProbeGround(across, raised + 0.05f, drop))
            {
                const Vector3 down = collision.MoveCapsule(across, { 0.0f, -drop, 0.0f }, colliderRadius, colliderHalfHeight, collider);
                if (HorizontalLength(down - start) > achieved + 0.001f)
                    moved = down;
            }
        }
    }

    // Rising into a ceiling stops the jump.
    if (delta.y > 0.0f && moved.y - start.y < delta.y * 0.5f)
        velocity.y = 0.0f;

    Position = moved;

    // Ground check. While walking (not jumping) it reaches a step down, which
    // keeps the player on stairs and slopes instead of skipping off them.
    const bool wasGrounded = grounded;
    float drop = 0.0f;
    const float probeDistance = wasGrounded && !jumped ? stepHeight : 0.05f;
    if (velocity.y <= 0.0f && ProbeGround(Position, probeDistance, drop))
    {
        grounded = true;
        velocity.y = 0.0f;
        if (drop > 0.01f)
            Position = collision.MoveCapsule(Position, { 0.0f, -drop, 0.0f }, colliderRadius, colliderHalfHeight, collider);
    }
    else
    {
        grounded = false;
    }
    if (wasGrounded && !grounded && !jumped)
        coyoteTime = kCoyoteTime;

    // Walls take the speed away instead of leaving it to build up against them,
    // and so does anything holding the player up that isn't walkable.
    const Vector3 actual = (Position - start) / dt;
    if (!grounded && velocity.y < 0.0f && actual.y > velocity.y)
        velocity.y = (std::min)(actual.y, 0.0f);
    if (HorizontalLength(actual) < HorizontalLength(velocity))
    {
        velocity.x = actual.x;
        velocity.z = actual.z;
    }
}

void EnginePlayerEntity::FixedUpdate(float fixedDt)
{
    if (movementMode == PlayerMovementMode::Walking)
    {
        previousPosition = Position;
        StepWalking(fixedDt);
    }

    // Follows at a velocity, which is what lets the capsule push dynamic bodies.
    if (game && game->collision && collider != 0)
        game->collision->SetTargetPosition(collider, Position, fixedDt);
}

void EnginePlayerEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    previousPosition = Position;
    velocity = {};
    grounded = false;
    SyncRuntimeCameraFromEntity(GetEyePosition());
    if (game && game->collision && collider != 0)
        game->collision->SetPosition(collider, Position);
}

void EnginePlayerEntity::SyncRuntimeCameraFromEntity(const Vector3& eye) const
{
    CameraEntity* activeCamera = game ? game->GetActiveCameraEntity() : nullptr;
    if (activeCamera && activeCamera->IsPlayerControlled())
    {
        activeCamera->SetWorldPose(eye, Rotation);
        if (game->mainCamera)
            activeCamera->ApplyToCamera(*game->mainCamera);
        return;
    }

    if (!activeCamera && game && game->mainCamera)
        ApplyTransformToCamera(*game->mainCamera, eye, Rotation);
}

bool EnginePlayerEntity::SyncEntityFromActiveCamera()
{
    const Vector3 eyeToCenter = { 0.0f, -eyeOffset, 0.0f };
    const CameraEntity* activeCamera = game ? game->GetActiveCameraEntity() : nullptr;
    if (activeCamera && activeCamera->IsPlayerControlled())
    {
        Position = activeCamera->Position + eyeToCenter;
        Rotation = activeCamera->Rotation;
        return true;
    }

    if (!activeCamera && game && game->mainCamera)
    {
        Position = Vector3{ game->mainCamera->x, game->mainCamera->y, game->mainCamera->z } + eyeToCenter;
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

#include "ent_camera.h"
#include "entity_property_utils.h"
#include "game_context.h"

void CameraEntity::Start()
{
    if (activateOnStart)
        SetActive(true);
}

void CameraEntity::Update(float dt)
{
    (void)dt;
    if (!IsActive() || !game || !game->mainCamera)
        return;

    ApplyToCamera(*game->mainCamera);
}

bool CameraEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "Active" || name == "Enabled" || name == "AutoActivate")
    {
        activateOnStart = entityprops::ParseBool(value, activateOnStart);
        if (game)
            SetActive(activateOnStart);
        return true;
    }

    if (name == "PlayerControlled")
    {
        SetPlayerControlled(entityprops::ParseBool(value, playerControlled));
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

void CameraEntity::SetActive(bool active)
{
    activateOnStart = active;
    if (!game)
        return;

    if (active)
    {
        game->SetActiveCameraEntity(GetEntityId());
        return;
    }

    if (game->IsCameraEntityActive(GetEntityId()))
        game->SetActiveCameraEntity(0);
}

bool CameraEntity::IsActive() const
{
    return game && game->IsCameraEntityActive(GetEntityId());
}

void CameraEntity::SetPlayerControlled(bool enabled)
{
    playerControlled = enabled;
}

void CameraEntity::ApplyToCamera(Camera& camera) const
{
    camera.x = Position.x;
    camera.y = Position.y;
    camera.z = Position.z;
    camera.pitch = Rotation.x;
    camera.yaw = Rotation.y;
}

void CameraEntity::SetFromCamera(const Camera& camera)
{
    Position = { camera.x, camera.y, camera.z };
    Rotation = { camera.pitch, camera.yaw, 0.0f };
}

void CameraEntity::SetWorldPose(const Vector3& position, const Vector3& rotation)
{
    Position = position;
    Rotation = rotation;
}

void CameraEntity::OnDestroy()
{
    if (game && game->IsCameraEntityActive(GetEntityId()))
        game->SetActiveCameraEntity(0);
}

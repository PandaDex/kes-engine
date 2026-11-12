#include "ent_fog_volume.h"
#include "camera.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include <algorithm>
#include <cmath>

namespace
{
    static float ClampStepCount(float value)
    {
        return ClampFloat(value, 1.0f, 64.0f);
    }

    static float ComputeSelectionVolume(const Vector3& size)
    {
        const float sx = std::fabs(size.x);
        const float sy = std::fabs(size.y);
        const float sz = std::fabs(size.z);
        return sx * sy * sz;
    }

    static bool ShouldReplaceFog(const FogSettings& current, const FogSettings& candidate)
    {
        if (!current.active)
            return true;
        if (candidate.priority != current.priority)
            return candidate.priority > current.priority;
        if (std::fabs(candidate.selectionVolume - current.selectionVolume) > 0.0001f)
            return candidate.selectionVolume < current.selectionVolume;
        return candidate.ownerEntityId < current.ownerEntityId;
    }
}

FogVolumeEntity::FogVolumeEntity()
{
    fogData.color[0] = 0.45f;
    fogData.color[1] = 0.49f;
    fogData.color[2] = 0.55f;
    fogData.nearDistance = 6.0f;
    fogData.farDistance = 18.0f;
    fogData.stepCount = 10.0f;
    fogData.active = true;
}

void FogVolumeEntity::Start()
{
    SubmitFogIfCameraInside();
}

void FogVolumeEntity::Update(float dt)
{
    (void)dt;
    SubmitFogIfCameraInside();
}

bool FogVolumeEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "Color" || name == "FogColor")
    {
        const Vector3 color = entityprops::ParseVector3(value, { fogData.color[0], fogData.color[1], fogData.color[2] });
        fogData.color[0] = color.x;
        fogData.color[1] = color.y;
        fogData.color[2] = color.z;
        return true;
    }

    if (name == "Near" || name == "NearDistance" || name == "Start" || name == "StartDistance")
    {
        fogData.nearDistance = std::max(entityprops::ParseFloat(value, fogData.nearDistance), 0.0f);
        return true;
    }

    if (name == "Far" || name == "FarDistance" || name == "End" || name == "EndDistance")
    {
        fogData.farDistance = std::max(entityprops::ParseFloat(value, fogData.farDistance), 0.0f);
        return true;
    }

    if (name == "Steps" || name == "StepCount")
    {
        fogData.stepCount = ClampStepCount(entityprops::ParseFloat(value, fogData.stepCount));
        return true;
    }

    if (name == "Priority")
    {
        fogData.priority = static_cast<int32_t>(entityprops::ParseFloat(value, static_cast<float>(fogData.priority)));
        return true;
    }

    if (name == "Size" || name == "Scale")
    {
        size = entityprops::ParseVector3(value, size);
        return true;
    }

    if (name == "Active" || name == "Enabled")
    {
        fogData.active = entityprops::ParseBool(value, fogData.active);
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

bool FogVolumeEntity::ContainsPoint(const Vector3& point) const
{
    const Vector3 half = {
        std::fabs(size.x) * 0.5f,
        std::fabs(size.y) * 0.5f,
        std::fabs(size.z) * 0.5f
    };
    const Vector3 delta = point - Position;
    return std::fabs(delta.x) <= half.x &&
        std::fabs(delta.y) <= half.y &&
        std::fabs(delta.z) <= half.z;
}

void FogVolumeEntity::SubmitFogIfCameraInside()
{
    if (!game || !game->scene || !game->mainCamera || !fogData.active)
        return;

    const Vector3 cameraPosition = CameraGetPosition(*game->mainCamera);
    if (!ContainsPoint(cameraPosition))
        return;

    FogSettings candidate = fogData;
    candidate.ownerEntityId = GetEntityId();
    candidate.selectionVolume = ComputeSelectionVolume(size);
    if (candidate.farDistance < candidate.nearDistance)
        candidate.farDistance = candidate.nearDistance;

    FogSettings& sceneFog = game->scene->GetFog();
    if (ShouldReplaceFog(sceneFog, candidate))
        sceneFog = candidate;
}

void FogVolumeEntity::OnDestroy()
{
    if (!game || !game->scene)
        return;

    FogSettings& sceneFog = game->scene->GetFog();
    if (sceneFog.ownerEntityId == GetEntityId())
        sceneFog = {};
}

const MapScriptPorts& FogVolumeEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = { { { "Enable" }, { "Disable" }, { "Toggle" } }, {} };
    return ports;
}

bool FogVolumeEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    (void)call;
    if (input != "Enable" && input != "Disable" && input != "Toggle")
        return false;
    fogData.active = input == "Enable" || (input == "Toggle" && !fogData.active);
    return true;
}

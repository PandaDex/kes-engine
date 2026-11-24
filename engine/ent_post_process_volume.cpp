#include "ent_post_process_volume.h"
#include "camera.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include <algorithm>
#include <cmath>

void PostProcessVolumeEntity::Start()
{
    Submit();
}

void PostProcessVolumeEntity::Update(float dt)
{
    (void)dt;
    Submit();
}

bool PostProcessVolumeEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    PostProcessSettings& s = layer.settings;
    // An empty value leaves the setting to the layers below.
    const bool empty = value.find_first_not_of(" \t\"") == std::string::npos;
    struct FloatField
    {
        const char* name;
        float* value;
        uint32_t bit;
        float minValue;
        float maxValue;
    };
    const FloatField floats[] = {
        { "ExposureCompensation", &s.exposureCompensation, PostProcessOverride_ExposureCompensation, -16.0f, 16.0f },
        { "AutoExposureMin", &s.autoExposureMin, PostProcessOverride_AutoExposureMin, -16.0f, 16.0f },
        { "AutoExposureMax", &s.autoExposureMax, PostProcessOverride_AutoExposureMax, -16.0f, 16.0f },
        { "AdaptationSpeedUp", &s.adaptationSpeedUp, PostProcessOverride_AdaptationSpeedUp, 0.0f, 100.0f },
        { "AdaptationSpeedDown", &s.adaptationSpeedDown, PostProcessOverride_AdaptationSpeedDown, 0.0f, 100.0f },
        { "BloomIntensity", &s.bloomIntensity, PostProcessOverride_BloomIntensity, 0.0f, 1.0f },
        { "BloomRadius", &s.bloomRadius, PostProcessOverride_BloomRadius, 0.0f, 4.0f },
        { "VignetteIntensity", &s.vignetteIntensity, PostProcessOverride_VignetteIntensity, 0.0f, 1.0f },
        { "VignetteSmoothness", &s.vignetteSmoothness, PostProcessOverride_VignetteSmoothness, 0.0f, 1.0f },
    };
    for (const FloatField& field : floats)
    {
        if (name == field.name)
        {
            if (empty)
                return true;
            *field.value = std::clamp(entityprops::ParseFloat(value, *field.value), field.minValue, field.maxValue);
            layer.overrides |= field.bit;
            return true;
        }
    }

    if (name == "AutoExposure")
    {
        if (empty)
            return true;
        s.autoExposure = entityprops::ParseBool(value, s.autoExposure);
        layer.overrides |= PostProcessOverride_AutoExposure;
        return true;
    }
    if (name == "VignetteColor")
    {
        if (empty)
            return true;
        const Vector3 color = entityprops::ParseVector3(value, { s.vignetteColor[0], s.vignetteColor[1], s.vignetteColor[2] });
        s.vignetteColor[0] = color.x;
        s.vignetteColor[1] = color.y;
        s.vignetteColor[2] = color.z;
        layer.overrides |= PostProcessOverride_VignetteColor;
        return true;
    }
    if (name == "Priority")
    {
        layer.priority = static_cast<int32_t>(entityprops::ParseFloat(value, static_cast<float>(layer.priority)));
        return true;
    }
    if (name == "Weight")
    {
        layer.weight = std::clamp(entityprops::ParseFloat(value, layer.weight), 0.0f, 1.0f);
        return true;
    }
    if (name == "BlendDistance")
    {
        blendDistance = std::max(entityprops::ParseFloat(value, blendDistance), 0.0f);
        return true;
    }
    if (name == "Unbound" || name == "Global")
    {
        unbound = entityprops::ParseBool(value, unbound);
        return true;
    }
    if (name == "Size" || name == "Scale")
    {
        size = entityprops::ParseVector3(value, size);
        return true;
    }
    if (name == "Active" || name == "Enabled")
    {
        active = entityprops::ParseBool(value, active);
        return true;
    }
    return Entity::ApplyProperty(name, value);
}

float PostProcessVolumeEntity::ComputeCameraWeight() const
{
    if (unbound)
        return 1.0f;
    const Vector3 cameraPosition = CameraGetPosition(*game->mainCamera);
    const Vector3 delta = cameraPosition - Position;
    // Distance from the camera to the box (0 inside).
    const float dx = std::max(std::fabs(delta.x) - std::fabs(size.x) * 0.5f, 0.0f);
    const float dy = std::max(std::fabs(delta.y) - std::fabs(size.y) * 0.5f, 0.0f);
    const float dz = std::max(std::fabs(delta.z) - std::fabs(size.z) * 0.5f, 0.0f);
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (distance <= 0.0f)
        return 1.0f;
    if (distance >= blendDistance)
        return 0.0f;
    return 1.0f - distance / blendDistance;
}

void PostProcessVolumeEntity::Submit()
{
    if (!active || !game || !game->scene || !game->mainCamera || layer.overrides == 0)
        return;
    const float weight = layer.weight * ComputeCameraWeight();
    if (weight <= 0.0f)
        return;
    PostProcessLayer submitted = layer;
    submitted.weight = weight;
    game->scene->SubmitPostProcessLayer(submitted);
}

const MapScriptPorts& PostProcessVolumeEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = { { { "Enable" }, { "Disable" }, { "Toggle" }, { "SetWeight", "float" } }, {} };
    return ports;
}

bool PostProcessVolumeEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    if (input == "SetWeight")
    {
        layer.weight = std::clamp(entityprops::ParseFloat(call.value, layer.weight), 0.0f, 1.0f);
        return true;
    }
    if (input != "Enable" && input != "Disable" && input != "Toggle")
        return false;
    active = input == "Enable" || (input == "Toggle" && !active);
    return true;
}

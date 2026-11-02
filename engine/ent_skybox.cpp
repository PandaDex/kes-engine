#include "ent_skybox.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include <algorithm>

void SkyboxEntity::Start()
{
    SyncSkyFromEntity();
    PushSkyToScene();
}

void SkyboxEntity::Update(float dt)
{
    (void)dt;
    SyncSkyFromEntity();
    PushSkyToScene();
}

bool SkyboxEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "TopColor")
    {
        const Vector3 color = entityprops::ParseVector3(value, { skyData.topColor[0], skyData.topColor[1], skyData.topColor[2] });
        SetTopColor(color.x, color.y, color.z);
        return true;
    }

    if (name == "HorizonColor")
    {
        const Vector3 color = entityprops::ParseVector3(value, { skyData.horizonColor[0], skyData.horizonColor[1], skyData.horizonColor[2] });
        SetHorizonColor(color.x, color.y, color.z);
        return true;
    }

    if (name == "BottomColor")
    {
        const Vector3 color = entityprops::ParseVector3(value, { skyData.bottomColor[0], skyData.bottomColor[1], skyData.bottomColor[2] });
        SetBottomColor(color.x, color.y, color.z);
        return true;
    }

    if (name == "SunColor")
    {
        const Vector3 color = entityprops::ParseVector3(value, { skyData.sunColor[0], skyData.sunColor[1], skyData.sunColor[2] });
        SetSunColor(color.x, color.y, color.z);
        return true;
    }

    if (name == "SunSize")
    {
        SetSunSize(entityprops::ParseFloat(value, skyData.sunSize));
        return true;
    }

    if (name == "SunIntensity")
    {
        SetSunIntensity(entityprops::ParseFloat(value, skyData.sunIntensity));
        return true;
    }

    if (name == "HorizonExponent")
    {
        SetHorizonExponent(entityprops::ParseFloat(value, skyData.horizonExponent));
        return true;
    }

    if (name == "Active" || name == "Enabled")
    {
        SetActive(entityprops::ParseBool(value, skyData.active));
        return true;
    }

    if (name == "AmbientIntensity")
    {
        skyData.ambientIntensity = std::max(entityprops::ParseFloat(value, skyData.ambientIntensity), 0.0f);
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

void SkyboxEntity::SetTopColor(float r, float g, float b)
{
    skyData.topColor[0] = r;
    skyData.topColor[1] = g;
    skyData.topColor[2] = b;
}

void SkyboxEntity::SetHorizonColor(float r, float g, float b)
{
    skyData.horizonColor[0] = r;
    skyData.horizonColor[1] = g;
    skyData.horizonColor[2] = b;
}

void SkyboxEntity::SetBottomColor(float r, float g, float b)
{
    skyData.bottomColor[0] = r;
    skyData.bottomColor[1] = g;
    skyData.bottomColor[2] = b;
}

void SkyboxEntity::SetSunColor(float r, float g, float b)
{
    skyData.sunColor[0] = r;
    skyData.sunColor[1] = g;
    skyData.sunColor[2] = b;
}

void SkyboxEntity::SetSunSize(float degrees)
{
    skyData.sunSize = std::max(degrees, 0.1f);
}

void SkyboxEntity::SetSunIntensity(float intensity)
{
    skyData.sunIntensity = std::max(intensity, 0.0f);
}

void SkyboxEntity::SetHorizonExponent(float exponent)
{
    skyData.horizonExponent = std::max(exponent, 0.05f);
}

void SkyboxEntity::SetActive(bool active)
{
    skyData.active = active;
}

void SkyboxEntity::SyncSkyFromEntity()
{
    const Vector3 sunDirection = Vector3DirectionFromRotation(Rotation);
    skyData.sunDirection[0] = sunDirection.x;
    skyData.sunDirection[1] = sunDirection.y;
    skyData.sunDirection[2] = sunDirection.z;
    skyData.ownerEntityId = GetEntityId();
}

void SkyboxEntity::PushSkyToScene()
{
    if (!game || !game->scene)
        return;

    game->scene->GetSky() = skyData;
}

void SkyboxEntity::OnDestroy()
{
    if (!game || !game->scene)
        return;

    SkySettings& sceneSky = game->scene->GetSky();
    if (sceneSky.ownerEntityId == GetEntityId())
        sceneSky = {};
}

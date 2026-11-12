#include "ent_light.h"
#include "entity_property_utils.h"
#include "game_context.h"

LightEntity::LightEntity(LightType type)
{
    lightData.type = type;
}

LightEntity::~LightEntity()
{
}

void LightEntity::Start()
{
    // Unless the map says otherwise, directional and spot lights cast
    // shadows; point lights don't (six shadow passes each).
    if (!castShadowsSet)
        lightData.castShadows = lightData.type != LightType::Point;
    SyncLightTransformFromEntity();
    if (game && game->scene)
    {
        lightId = game->scene->CreateLight(lightData);
    }
}

void LightEntity::Update(float dt)
{
    (void)dt;
    SyncLightTransformFromEntity();
    if (game && game->scene)
    {
        Light* liveLight = game->scene->FindLight(lightId);
        if (liveLight)
        {
            const uint32_t existingId = liveLight->id;
            *liveLight = lightData;
            liveLight->id = existingId;
        }
    }
}

void LightEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    SyncLightTransformFromEntity();
}

void LightEntity::SetPosition(float x, float y, float z)
{
    Position = { x, y, z };
    SyncLightTransformFromEntity();
}

void LightEntity::SetRotation(float pitch, float yaw, float roll)
{
    Rotation = { pitch, yaw, roll };
    SyncLightTransformFromEntity();
}

void LightEntity::SetColor(float r, float g, float b)
{
    lightData.color[0] = r;
    lightData.color[1] = g;
    lightData.color[2] = b;
}

void LightEntity::SetIntensity(float intensity)
{
    lightData.intensity = intensity;
}

void LightEntity::SetRange(float range)
{
    lightData.range = range;
}

void LightEntity::SetSpotAngle(float angle)
{
    lightData.spotAngle = angle;
}

void LightEntity::SetActive(bool active)
{
    lightData.active = active;
}

bool LightEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "Type")
    {
        const std::string type = entityprops::ParseString(value);
        if (type == "Directional")
            lightData.type = LightType::Directional;
        else if (type == "Point")
            lightData.type = LightType::Point;
        else if (type == "Spot")
            lightData.type = LightType::Spot;
        else
            return false;
        return true;
    }

    if (name == "Color")
    {
        const Vector3 color = entityprops::ParseVector3(value, { lightData.color[0], lightData.color[1], lightData.color[2] });
        SetColor(color.x, color.y, color.z);
        return true;
    }

    if (name == "Intensity")
    {
        SetIntensity(entityprops::ParseFloat(value, lightData.intensity));
        return true;
    }

    if (name == "Range")
    {
        SetRange(entityprops::ParseFloat(value, lightData.range));
        return true;
    }

    if (name == "SpotAngle")
    {
        SetSpotAngle(entityprops::ParseFloat(value, lightData.spotAngle));
        return true;
    }

    if (name == "CastShadows")
    {
        // Empty = decide by light type in Start().
        castShadowsSet = !entityprops::Trim(value).empty();
        if (castShadowsSet)
            lightData.castShadows = entityprops::ParseBool(value, lightData.castShadows);
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

void LightEntity::SyncLightTransformFromEntity()
{
    lightData.transform.position = Position;
    lightData.transform.rotation = Rotation;
}

void LightEntity::OnDestroy()
{
    if (game && game->scene)
    {
        game->scene->RemoveLight(lightId);
    }
}

const MapScriptPorts& LightEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = {
        { { "TurnOn" }, { "TurnOff" }, { "Toggle" }, { "SetIntensity", "float" } },
        {},
    };
    return ports;
}

bool LightEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    if (input == "TurnOn" || input == "TurnOff" || input == "Toggle")
    {
        SetActive(input == "TurnOn" || (input == "Toggle" && !lightData.active));
        return true;
    }
    if (input == "SetIntensity")
    {
        SetIntensity(entityprops::ParseFloat(call.value, lightData.intensity));
        return true;
    }
    return false;
}

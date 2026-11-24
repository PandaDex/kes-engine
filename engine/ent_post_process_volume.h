#pragma once
#include "entity.h"
#include "map_script.h"
#include "scene.h"

// Post-process settings for the camera. Unbound volumes apply everywhere;
// bound ones while the camera is inside the box, fading in over BlendDistance
// meters outside it. Only the properties the map sets are overridden.
class PostProcessVolumeEntity : public Entity
    , public IMapScriptable
{
public:
    PostProcessVolumeEntity() = default;
    ~PostProcessVolumeEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;
    const MapScriptPorts& GetMapScriptPorts() const override;
    bool AcceptInput(const std::string& input, const MapScriptCall& call) override;

    PostProcessLayer& GetLayer() { return layer; }
    void SetActive(bool value) { active = value; }
    bool IsActive() const { return active; }

private:
    float ComputeCameraWeight() const;
    void Submit();

    Vector3 size = { 1.0f, 1.0f, 1.0f };
    bool unbound = false;
    float blendDistance = 0.0f;
    bool active = true;
    PostProcessLayer layer = { {}, 0u, 0, 1.0f };
};

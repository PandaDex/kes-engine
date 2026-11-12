#pragma once
#include "entity.h"
#include "map_script.h"
#include "scene.h"

class FogVolumeEntity : public Entity, public IMapScriptable
{
public:
    FogVolumeEntity();
    ~FogVolumeEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;
    const MapScriptPorts& GetMapScriptPorts() const override;
    bool AcceptInput(const std::string& input, const MapScriptCall& call) override;

private:
    bool ContainsPoint(const Vector3& point) const;
    void SubmitFogIfCameraInside();
    void OnDestroy() override;

    Vector3 size = { 1.0f, 1.0f, 1.0f };
    FogSettings fogData = {};
};

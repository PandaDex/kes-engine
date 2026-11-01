#pragma once
#include "entity.h"
#include "scene.h"

class FogVolumeEntity : public Entity
{
public:
    FogVolumeEntity();
    ~FogVolumeEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

private:
    bool ContainsPoint(const Vector3& point) const;
    void SubmitFogIfCameraInside();
    void OnDestroy() override;

    Vector3 size = { 1.0f, 1.0f, 1.0f };
    FogSettings fogData = {};
};

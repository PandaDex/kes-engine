#pragma once
#include "entity.h"
#include "scene.h"

class LightEntity : public Entity
{
public:
    LightEntity(LightType type = LightType::Point);
    ~LightEntity() override;

    void Start() override;
    void Update(float dt) override;
    void Teleport(const Vector3& newPosition) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

    void SetPosition(float x, float y, float z);
    void SetRotation(float pitch, float yaw, float roll);
    void SetColor(float r, float g, float b);
    void SetIntensity(float intensity);
    void SetRange(float range);
    void SetSpotAngle(float angle);
    void SetActive(bool active);

    Light& GetData() { return lightData; }
    const Light& GetData() const { return lightData; }

private:
    void SyncLightTransformFromEntity();
    void OnDestroy() override;

    uint32_t lightId = 0;
    Light lightData;
};

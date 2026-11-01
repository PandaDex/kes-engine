#pragma once
#include "entity.h"
#include "scene.h"

class SkyboxEntity : public Entity
{
public:
    SkyboxEntity() = default;
    ~SkyboxEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

    void SetTopColor(float r, float g, float b);
    void SetHorizonColor(float r, float g, float b);
    void SetBottomColor(float r, float g, float b);
    void SetSunColor(float r, float g, float b);
    void SetSunSize(float degrees);
    void SetSunIntensity(float intensity);
    void SetHorizonExponent(float exponent);
    void SetActive(bool active);

    SkySettings& GetData() { return skyData; }
    const SkySettings& GetData() const { return skyData; }

private:
    void SyncSkyFromEntity();
    void PushSkyToScene();
    void OnDestroy() override;

    SkySettings skyData = {};
};

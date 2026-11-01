#pragma once
#include "camera.h"
#include "entity.h"

class CameraEntity : public Entity
{
public:
    CameraEntity() = default;
    ~CameraEntity() override = default;

    void Start() override;
    void Update(float dt) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

    void SetActive(bool active);
    bool WantsToBeActiveOnStart() const { return activateOnStart; }
    bool IsActive() const;

    void SetPlayerControlled(bool enabled);
    bool IsPlayerControlled() const { return playerControlled; }

    void ApplyToCamera(Camera& camera) const;
    void SetFromCamera(const Camera& camera);
    void SetWorldPose(const Vector3& position, const Vector3& rotation);

private:
    void OnDestroy() override;

    bool activateOnStart = true;
    bool playerControlled = false;
};

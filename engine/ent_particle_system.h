#pragma once
#include <string>
#include <utility>
#include <vector>
#include "entity.h"
#include "map_script.h"
#include "particle_world.h"

// Places a .kfx particle system in the map. Follows the entity's position and
// rotation (local-space emitters move with it).
//   System        content path of the .kfx
//   AutoActivate  start when the map starts (default true)
//   Scale         uniform scale of the system
//   User.<name>   initial value of a user parameter
class ParticleSystemEntity : public Entity
    , public IMapScriptable
{
public:
    void Start() override;
    void Update(float dt) override;
    void Teleport(const Vector3& newPosition) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;
    const MapScriptPorts& GetMapScriptPorts() const override;
    bool AcceptInput(const std::string& input, const MapScriptCall& call) override;

    void Activate(bool reset = true);
    void Deactivate();
    bool SetUserFloat(const std::string& name, float value);
    ParticleSystemInstance* GetInstance();

private:
    void OnDestroy() override;

    std::string systemPath;
    bool autoActivate = true;
    float scale = 1.0f;
    std::vector<std::pair<std::string, float>> userValues;
    ParticleSystemHandle handle = 0;
    bool finishedFired = false;
};

#pragma once
#include "entity.h"
#include "map_script.h"

// MapScript relay: forwards Trigger to OnTrigger, so one event can drive many links and be
// switched off as a group.
// Properties: StartDisabled
class LogicRelayEntity : public Entity, public IMapScriptable
{
public:
    bool ApplyProperty(const std::string& name, const std::string& value) override;
    const MapScriptPorts& GetMapScriptPorts() const override;
    bool AcceptInput(const std::string& input, const MapScriptCall& call) override;

    bool IsEnabled() const { return enabled; }

private:
    bool enabled = true;
};

// Fires OnMapSpawn once when the map has started.
class LogicAutoEntity : public Entity, public IMapScriptable
{
public:
    void Start() override;
    const MapScriptPorts& GetMapScriptPorts() const override;
    bool AcceptInput(const std::string& input, const MapScriptCall& call) override;
};

#include "ent_logic.h"
#include "entity_property_utils.h"

bool LogicRelayEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "StartDisabled")
    {
        enabled = !entityprops::ParseBool(value, !enabled);
        return true;
    }
    return Entity::ApplyProperty(name, value);
}

const MapScriptPorts& LogicRelayEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = {
        { { "Trigger" }, { "Enable" }, { "Disable" }, { "Toggle" } },
        { { "OnTrigger" } },
    };
    return ports;
}

bool LogicRelayEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    if (input == "Trigger")
    {
        if (enabled)
            FireOutput("OnTrigger", call.activator, call.value);
        return true;
    }
    if (input == "Enable" || input == "Disable" || input == "Toggle")
    {
        enabled = input == "Enable" || (input == "Toggle" && !enabled);
        return true;
    }
    return false;
}

void LogicAutoEntity::Start()
{
    FireOutput("OnMapSpawn");
}

const MapScriptPorts& LogicAutoEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = { {}, { { "OnMapSpawn" } } };
    return ports;
}

bool LogicAutoEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    (void)input;
    (void)call;
    return false;
}

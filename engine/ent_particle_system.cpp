#include "ent_particle_system.h"
#include <cstdlib>
#include "entity_property_utils.h"
#include "game_context.h"
#include "logger.h"

ParticleSystemInstance* ParticleSystemEntity::GetInstance()
{
    return game && game->particles ? game->particles->Find(handle) : nullptr;
}

void ParticleSystemEntity::Start()
{
    if (!game || !game->particles || systemPath.empty())
        return;
    handle = game->particles->Spawn(systemPath, Position, Rotation, false, { scale, scale, scale });
    ParticleSystemInstance* instance = GetInstance();
    if (!instance)
        return;
    for (const auto& user : userValues)
    {
        if (!instance->SetUserFloat(user.first, user.second))
            EngineLogf("%s has no user parameter \"%s\".", systemPath.c_str(), user.first.c_str());
    }
    if (!autoActivate)
    {
        instance->Deactivate();
        instance->Clear();
        finishedFired = true; // only report systems that ran
    }
}

void ParticleSystemEntity::Update(float dt)
{
    (void)dt;
    ParticleSystemInstance* instance = GetInstance();
    if (!instance)
        return;
    instance->SetTransform(Position, Rotation, { scale, scale, scale });
    if (instance->IsComplete() && instance->GetData() && !finishedFired)
    {
        finishedFired = true;
        FireOutput("OnFinished");
    }
}

void ParticleSystemEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    if (ParticleSystemInstance* instance = GetInstance())
        instance->SetTransform(Position, Rotation, { scale, scale, scale }, true);
}

void ParticleSystemEntity::Activate(bool reset)
{
    if (ParticleSystemInstance* instance = GetInstance())
    {
        instance->SetTransform(Position, Rotation, { scale, scale, scale }, true);
        instance->Activate(reset);
        finishedFired = false;
    }
}

void ParticleSystemEntity::Deactivate()
{
    if (ParticleSystemInstance* instance = GetInstance())
        instance->Deactivate();
}

bool ParticleSystemEntity::SetUserFloat(const std::string& name, float value)
{
    ParticleSystemInstance* instance = GetInstance();
    return instance && instance->SetUserFloat(name, value);
}

bool ParticleSystemEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "System")
    {
        systemPath = entityprops::ParseString(value);
        return true;
    }
    if (name == "AutoActivate")
    {
        autoActivate = entityprops::ParseBool(value, autoActivate);
        return true;
    }
    if (name == "Scale")
    {
        scale = entityprops::ParseFloat(value, scale);
        return true;
    }
    if (name.rfind("User.", 0) == 0)
    {
        userValues.emplace_back(name.substr(5), entityprops::ParseFloat(value, 0.0f));
        return true;
    }
    return Entity::ApplyProperty(name, value);
}

const MapScriptPorts& ParticleSystemEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = {
        { { "Activate" }, { "Deactivate" }, { "Toggle" }, { "Restart" }, { "Kill" }, { "SetUserParameter", "string" } },
        { { "OnFinished" } },
    };
    return ports;
}

bool ParticleSystemEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    ParticleSystemInstance* instance = GetInstance();
    if (input == "Activate")
        Activate(false);
    else if (input == "Restart")
        Activate(true);
    else if (input == "Deactivate")
        Deactivate();
    else if (input == "Toggle")
    {
        if (instance && instance->IsActive())
            Deactivate();
        else
            Activate(false);
    }
    else if (input == "Kill")
    {
        if (instance)
        {
            instance->Deactivate();
            instance->Clear();
        }
    }
    else if (input == "SetUserParameter")
    {
        // "Name value"
        const std::string text = entityprops::Trim(call.value);
        const size_t split = text.find_first_of(" =");
        if (split == std::string::npos)
            return false;
        const std::string name = text.substr(0, split);
        const float value = std::strtof(text.c_str() + split + 1, nullptr);
        if (!SetUserFloat(name, value))
            EngineLogf("SetUserParameter: no user parameter \"%s\".", name.c_str());
    }
    else
        return false;
    return true;
}

void ParticleSystemEntity::OnDestroy()
{
    if (game && game->particles && handle)
        game->particles->Destroy(handle);
    handle = 0;
}

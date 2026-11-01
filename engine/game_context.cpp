#include "game_context.h"
#include "audio_system.h"
#include "ent_camera.h"
#include "world.h"
#include "entity.h"
#include <cctype>
#include <cstdlib>
#include <vector>

namespace
{
    static std::string Trim(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace((unsigned char)value[start]))
            ++start;

        size_t end = value.size();
        while (end > start && std::isspace((unsigned char)value[end - 1]))
            --end;

        return value.substr(start, end - start);
    }

    static std::string Unquote(const std::string& value)
    {
        if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            return value;

        std::string out;
        out.reserve(value.size() - 2);

        for (size_t i = 1; i + 1 < value.size(); ++i)
        {
            if (value[i] == '\\' && i + 1 < value.size() - 1)
            {
                ++i;
                out.push_back(value[i]);
                continue;
            }

            out.push_back(value[i]);
        }

        return out;
    }

    static std::vector<std::string> SplitArguments(const std::string& value)
    {
        std::vector<std::string> args;
        std::string current;
        bool inQuotes = false;
        bool escape = false;

        for (char ch : value)
        {
            if (escape)
            {
                current.push_back(ch);
                escape = false;
                continue;
            }

            if (ch == '\\')
            {
                current.push_back(ch);
                escape = true;
                continue;
            }

            if (ch == '"')
            {
                current.push_back(ch);
                inQuotes = !inQuotes;
                continue;
            }

            if (ch == ',' && !inQuotes)
            {
                args.push_back(Trim(current));
                current.clear();
                continue;
            }

            current.push_back(ch);
        }

        if (!current.empty())
            args.push_back(Trim(current));

        return args;
    }
}

void GameContext::Play2DSound(const char* soundFile, float volume, float pitch)
{
    if (audio)
        audio->Play2D(soundFile, volume, pitch);
}

void GameContext::Play3DSound(const char* soundFile, const Vector3& location, float attenuation, float volume, float pitch)
{
    if (audio)
        audio->Play3D(location, attenuation, soundFile, volume, pitch);
}

bool GameContext::LoadMap(const char* mapName)
{
    if (!world || !mapName || !mapName[0])
        return false;

    world->QueueMapLoad(mapName);
    return true;
}

bool GameContext::SetActiveCameraEntity(uint64_t entityId)
{
    if (entityId == 0)
    {
        activeCameraEntityId = 0;
        return true;
    }

    if (!entities)
        return false;

    CameraEntity* cameraEntity = dynamic_cast<CameraEntity*>(entities->GetEntityById(entityId));
    if (!cameraEntity)
        return false;

    activeCameraEntityId = entityId;
    if (mainCamera)
        cameraEntity->ApplyToCamera(*mainCamera);
    return true;
}

bool GameContext::SetActiveCameraByTag(const std::string& tag)
{
    CameraEntity* cameraEntity = GetEntityOfClassWithTag<CameraEntity>(tag);
    if (!cameraEntity)
        return false;

    return SetActiveCameraEntity(cameraEntity->GetEntityId());
}

CameraEntity* GameContext::GetActiveCameraEntity()
{
    if (!entities || activeCameraEntityId == 0)
        return nullptr;

    return dynamic_cast<CameraEntity*>(entities->GetEntityById(activeCameraEntityId));
}

const CameraEntity* GameContext::GetActiveCameraEntity() const
{
    if (!entities || activeCameraEntityId == 0)
        return nullptr;

    return dynamic_cast<const CameraEntity*>(entities->GetEntityById(activeCameraEntityId));
}

bool GameContext::ExecuteCommand(const std::string& command, Entity* self, Entity* other)
{
    (void)self;
    (void)other;

    const std::string trimmed = Trim(command);
    if (trimmed.empty())
        return true;

    const std::string prefix = "game.";
    if (trimmed.rfind(prefix, 0) != 0)
        return false;

    const size_t openParen = trimmed.find('(');
    const size_t closeParen = trimmed.rfind(')');
    if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen)
        return false;

    const std::string functionName = Trim(trimmed.substr(prefix.size(), openParen - prefix.size()));
    const std::vector<std::string> args = SplitArguments(trimmed.substr(openParen + 1, closeParen - openParen - 1));

    if (functionName == "Play2DSound")
    {
        if (args.empty())
            return false;

        const std::string soundPath = Unquote(args[0]);
        const float volume = args.size() > 1 ? std::strtof(args[1].c_str(), nullptr) : 1.0f;
        const float pitch = args.size() > 2 ? std::strtof(args[2].c_str(), nullptr) : 1.0f;
        Play2DSound(soundPath.c_str(), volume, pitch);
        return true;
    }

    if (functionName == "LoadMap")
    {
        if (args.empty())
            return false;

        return LoadMap(Unquote(args[0]).c_str());
    }

    if (functionName == "SetActiveCamera")
    {
        if (args.empty())
            return false;

        return SetActiveCameraByTag(Unquote(args[0]));
    }

    return false;
}

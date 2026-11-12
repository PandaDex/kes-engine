#include "ent_audio_source.h"
#include "entity_property_utils.h"
#include "game_context.h"

AudioSourceEntity::AudioSourceEntity(const char* soundFilePath, bool useSpatial)
    : soundFile(soundFilePath ? soundFilePath : "")
    , spatial(useSpatial)
{
}

void AudioSourceEntity::Start()
{
    if (!soundFile.empty() && (loop || autoPlay))
        Play();
}

void AudioSourceEntity::Update(float dt)
{
    (void)dt;
    if (!game || !game->audio || voiceId == 0)
        return;

    if (spatial)
        game->audio->UpdateVoice3D(voiceId, Position, attenuation);
    game->audio->SetVoiceVolume(voiceId, volume);
    game->audio->SetVoicePitch(voiceId, pitch);
}

void AudioSourceEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    if (game && game->audio && voiceId != 0 && spatial)
        game->audio->UpdateVoice3D(voiceId, Position, attenuation);
}

void AudioSourceEntity::Play()
{
    if (!game || !game->audio || soundFile.empty())
        return;

    Stop();
    voiceId = spatial
        ? game->audio->Play3D(Position, attenuation, soundFile.c_str(), volume, pitch, loop)
        : game->audio->Play2D(soundFile.c_str(), volume, pitch, loop);
}

void AudioSourceEntity::Stop()
{
    if (game && game->audio && voiceId != 0)
        game->audio->StopVoice(voiceId);
    voiceId = 0;
}

bool AudioSourceEntity::IsPlaying() const
{
    return game && game->audio && voiceId != 0 && game->audio->IsVoicePlaying(voiceId);
}

void AudioSourceEntity::OnDestroy()
{
    Stop();
}

bool AudioSourceEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "Sound")
    {
        SetSoundFile(entityprops::ParseString(value).c_str());
        return true;
    }

    if (name == "Attenuation")
    {
        SetAttenuation(entityprops::ParseFloat(value, attenuation));
        return true;
    }

    if (name == "Volume")
    {
        SetVolume(entityprops::ParseFloat(value, volume));
        return true;
    }

    if (name == "Pitch")
    {
        SetPitch(entityprops::ParseFloat(value, pitch));
        return true;
    }

    if (name == "Loop")
    {
        SetLoop(entityprops::ParseBool(value, loop));
        return true;
    }

    if (name == "AutoPlay")
    {
        autoPlay = entityprops::ParseBool(value, autoPlay);
        return true;
    }

    if (name == "Spatial")
    {
        SetSpatial(entityprops::ParseBool(value, spatial));
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

const MapScriptPorts& AudioSourceEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = { { { "Play" }, { "Stop" }, { "Toggle" } }, {} };
    return ports;
}

bool AudioSourceEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    (void)call;
    if (input == "Play" || (input == "Toggle" && !IsPlaying()))
        Play();
    else if (input == "Stop" || input == "Toggle")
        Stop();
    else
        return false;
    return true;
}

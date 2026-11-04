#pragma once
#include <string>
#include "entity.h"
#include "audio_system.h"

class AudioSourceEntity : public Entity
{
public:
    AudioSourceEntity(const char* soundFile = nullptr, bool spatial = true);

    void Start() override;
    void Update(float dt) override;
    void Teleport(const Vector3& newPosition) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

    void SetPosition(const Vector3& newPosition) { Teleport(newPosition); }
    const Vector3& GetPosition() const { return Position; }

    void SetAttenuation(float value) { attenuation = value; }
    void SetVolume(float value) { volume = value; }
    void SetPitch(float value) { pitch = value; }
    void SetLoop(bool value) { loop = value; }
    void SetSpatial(bool value) { spatial = value; }
    void SetSoundFile(const char* path) { soundFile = path ? path : ""; }

    void Play();
    void Stop();
    bool IsPlaying() const;

private:
    void OnDestroy() override;

    std::string soundFile;
    float attenuation = 1.0f;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool autoPlay = false;   // looping sounds always start with the map
    bool spatial = true;
    AudioVoiceId voiceId = 0;
};

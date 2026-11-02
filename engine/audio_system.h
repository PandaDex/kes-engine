#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "engine_types.h"

using AudioVoiceId = uint32_t;

struct AudioListener
{
    Vector3 position = {};
    Vector3 forward = { 0.0f, 0.0f, 1.0f };
    Vector3 up = { 0.0f, 1.0f, 0.0f };
};

struct AudioClip
{
    SDL_AudioSpec spec = {};
    std::vector<float> samples;
    int channels = 0;
    int frameCount = 0;
};

struct AudioVoice
{
    AudioVoiceId id = 0;
    const AudioClip* clip = nullptr;
    float samplePosition = 0.0f;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool spatial = false;
    bool loop = false;
    float attenuation = 1.0f;
    Vector3 position = {};
    bool playing = false;
};

class AudioSystem
{
public:
    bool Init();
    void Shutdown();
    void Update(float dt);

    void SetListener(const AudioListener& newListener);
    const AudioListener& GetListener() const { return listener; }

    bool LoadClip(const char* soundFile);
    AudioVoiceId Play2D(const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false);
    AudioVoiceId Play3D(const Vector3& location, float attenuation, const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false);
    void StopVoice(AudioVoiceId id);
    bool IsVoicePlaying(AudioVoiceId id) const;
    void UpdateVoice3D(AudioVoiceId id, const Vector3& location, float attenuation);
    void SetVoicePitch(AudioVoiceId id, float pitch);
    void SetVoiceVolume(AudioVoiceId id, float volume);

    // Pauses the output device (e.g. while the game is paused); voices keep
    // their positions.
    void SetPaused(bool paused);
    bool IsPaused() const { return paused; }
    void StopAll();
    // False when there is no output device; the system then runs silent.
    bool HasOutput() const { return stream != nullptr; }

    static constexpr size_t kMaxVoices = 128;

private:
    static void SDLAudioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
    bool AddVoice(const AudioVoice& voice);
    void MixAudio(float* outSamples, int frameCount);
    const AudioClip* GetOrLoadClip(const char* soundFile);
    AudioVoice* FindVoice(AudioVoiceId id);
    const AudioVoice* FindVoice(AudioVoiceId id) const;
    void CleanupVoices();

private:
    SDL_AudioStream* stream = nullptr;   // bound to the default playback device
    SDL_AudioSpec deviceSpec = {};       // mixer format: F32 stereo 48 kHz
    std::vector<float> mixBuffer;        // audio thread scratch
    bool paused = false;
    AudioListener listener = {};
    std::unordered_map<std::string, AudioClip> clipCache;
    std::vector<AudioVoice> voices;
    AudioVoiceId nextVoiceId = 1;
    SDL_Mutex* mutex = nullptr;
};

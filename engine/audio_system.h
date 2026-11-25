#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "engine_types.h"

using AudioVoiceId = uint32_t;

enum class AudioBus : uint8_t
{
    Effects,
    Music,
    Ambient,
    Voice,
    Ui,
    Count
};
const char* AudioBusName(AudioBus bus);

struct AudioListener
{
    Vector3 position = {};
    Vector3 forward = { 0.0f, 0.0f, 1.0f };
    Vector3 up = { 0.0f, 1.0f, 0.0f };
};

float ComputeStereoPan(const AudioListener& listener, const Vector3& soundPosition);
float ComputeDistanceGain(float distance, float attenuation);

struct AudioClip
{
    int channels = 0;
    int frameCount = 0;
    int sampleRate = 0;
    std::vector<float> samples;
    std::vector<uint8_t> encoded;
    bool streamed = false;
};

// Audio pushed from code at runtime (voice chat, procedural sound) instead of
// read from a clip. The voice plays silence while starved and waits until
// `prebufferSeconds` are queued before (re)starting, so late packets become a
// short gap instead of crackle.
struct AudioStreamDesc
{
    int sampleRate = 48000;
    int channels = 1;
    float bufferSeconds = 1.0f;       // ring capacity; the oldest audio is dropped beyond it
    float prebufferSeconds = 0.06f;
    float maxLatencySeconds = 0.3f;   // more queued than this skips ahead to catch up
    float volume = 1.0f;
    AudioBus bus = AudioBus::Voice;
    bool spatial = false;
    Vector3 position = {};
    float attenuation = 1.0f;
};

struct AudioPcmQueue
{
    int channels = 0;
    int sampleRate = 0;
    std::vector<float> ring;
    uint64_t ringFrames = 0;
    uint64_t writeFrame = 0;   // total frames ever queued
    double readFrame = 0.0;    // same timeline as writeFrame
    uint64_t prebufferFrames = 0;
    uint64_t maxLatencyFrames = 0;
    bool buffering = true;
};

struct AudioVoice
{
    AudioVoiceId id = 0;
    const AudioClip* clip = nullptr;
    double samplePosition = 0.0;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool spatial = false;
    bool loop = false;
    bool ignoresPause = false;
    float attenuation = 1.0f;
    Vector3 position = {};
    bool playing = false;
    AudioBus bus = AudioBus::Effects;

    float fadeGain = 1.0f;
    float fadeTarget = 1.0f;
    float fadeRate = 0.0f;
    bool stopAfterFade = false;

    float appliedGain[2] = {};
    bool gainApplied = false;
    float loudness = 0.0f;

    std::vector<float> blocks[2];
    uint32_t blockIndex[2] = { UINT32_MAX, UINT32_MAX };
    uint32_t blockFrames[2] = {};
    int nextBlockSlot = 0;

    bool isStream = false;
    AudioPcmQueue pcm;
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
    AudioVoiceId Play2D(const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false,
        AudioBus bus = AudioBus::Effects);
    AudioVoiceId Play3D(const Vector3& location, float attenuation, const char* soundFile, float volume = 1.0f,
        float pitch = 1.0f, bool loop = false, AudioBus bus = AudioBus::Effects);
    AudioVoiceId PlayUi(const char* soundFile, float volume = 1.0f);
    AudioVoiceId PlayMusic(const char* soundFile, float volume = 1.0f, bool loop = true, float fadeSeconds = 1.0f);
    void StopMusic(float fadeSeconds = 1.0f);
    AudioVoiceId GetMusicVoice() const { return musicVoice; }

    // Streams live until StopVoice; feed them with QueueStreamSamples.
    AudioVoiceId PlayStream(const AudioStreamDesc& desc);
    // Interleaved frames in the stream's channel count. Returns frames queued.
    int QueueStreamSamples(AudioVoiceId id, const int16_t* samples, int frameCount);
    int QueueStreamSamples(AudioVoiceId id, const float* samples, int frameCount);
    int GetStreamQueuedFrames(AudioVoiceId id) const;

    void StopVoice(AudioVoiceId id, float fadeSeconds = 0.0f);
    bool IsVoicePlaying(AudioVoiceId id) const;
    void UpdateVoice3D(AudioVoiceId id, const Vector3& location, float attenuation);
    void SetVoicePitch(AudioVoiceId id, float pitch);
    void SetVoiceVolume(AudioVoiceId id, float volume);
    void FadeVoice(AudioVoiceId id, float gain, float seconds);

    void SetMasterVolume(float volume);
    float GetMasterVolume() const { return masterVolume; }
    void SetBusVolume(AudioBus bus, float volume);
    float GetBusVolume(AudioBus bus) const;

    void SetPaused(bool paused);
    bool IsPaused() const { return paused; }
    void StopAll();
    bool HasOutput() const { return stream != nullptr; }
    size_t GetVoiceCount() const;

    static constexpr size_t kMaxVoices = 256;
    static constexpr size_t kMaxMixedVoices = 64;
    static constexpr float kStreamMinSeconds = 8.0f;

private:
    static void SDLAudioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
    AudioVoiceId AddVoice(AudioVoice voice);
    void MixAudio(float* outSamples, int frameCount);
    void ComputeVoiceGains(const AudioVoice& voice, float outGains[2]) const;
    void MixVoice(AudioVoice& voice, const float targetGains[2], float* outSamples, int frameCount);
    void AdvanceVoice(AudioVoice& voice, double frames, float seconds);
    bool ReadFrame(AudioVoice& voice, int frame, float& left, float& right);
    void MixStreamVoice(AudioVoice& voice, const float targetGains[2], float* outSamples, int frameCount);
    void AdvanceStream(AudioVoice& voice, double frames);
    double GetVoiceStep(const AudioVoice& voice) const;
    template<typename T>
    int QueueStream(AudioVoiceId id, const T* samples, int frameCount);
    const AudioClip* GetOrLoadClip(const char* soundFile, bool forceStream = false);
    AudioVoice* FindVoice(AudioVoiceId id);
    const AudioVoice* FindVoice(AudioVoiceId id) const;
    void CleanupVoices();

private:
    SDL_AudioStream* stream = nullptr;
    SDL_AudioSpec deviceSpec = {};
    std::vector<float> mixBuffer;
    std::vector<size_t> mixOrder;
    std::vector<float> mixGains;
    bool paused = false;
    float masterVolume = 1.0f;
    float busVolumes[static_cast<size_t>(AudioBus::Count)] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    AudioListener listener = {};
    std::unordered_map<std::string, AudioClip> clipCache;
    std::vector<AudioVoice> voices;
    AudioVoiceId nextVoiceId = 1;
    AudioVoiceId musicVoice = 0;
    SDL_Mutex* mutex = nullptr;
};

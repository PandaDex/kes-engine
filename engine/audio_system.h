#pragma once
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "engine_types.h"

using AudioVoiceId = uint32_t;

// Volume groups. Every voice plays on one bus; the bus volume and the master
// volume multiply its own (GameSettings stores them for the player).
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

// -1 = fully left, 1 = fully right of the listener.
float ComputeStereoPan(const AudioListener& listener, const Vector3& soundPosition);
// Distance gain of a 3D sound: 1 / (1 + distance * attenuation), faded to
// exactly 0 where that drops under 1% (-40 dB), so far sounds cost nothing.
float ComputeDistanceGain(float distance, float attenuation);

struct AudioClip
{
    int channels = 0;
    int frameCount = 0;
    int sampleRate = 0;
    std::vector<float> samples;     // resident clips, converted to the mixer format
    std::vector<uint8_t> encoded;   // streamed clips: QOA, decoded while playing
    bool streamed = false;
};

struct AudioVoice
{
    AudioVoiceId id = 0;
    const AudioClip* clip = nullptr;
    double samplePosition = 0.0;   // in clip frames; a float stops advancing at 2^24 frames
    float volume = 1.0f;
    float pitch = 1.0f;
    bool spatial = false;
    bool loop = false;
    bool ignoresPause = false;
    float attenuation = 1.0f;
    Vector3 position = {};
    bool playing = false;
    AudioBus bus = AudioBus::Effects;

    // Fades: fadeGain moves toward fadeTarget at fadeRate per second; a
    // voice that fades to 0 with stopAfterFade ends.
    float fadeGain = 1.0f;
    float fadeTarget = 1.0f;
    float fadeRate = 0.0f;
    bool stopAfterFade = false;

    // Mixer state (audio thread).
    float appliedGain[2] = {};   // last gains used, ramped toward the new ones
    bool gainApplied = false;
    float loudness = 0.0f;       // last computed max channel gain, for stealing

    // Streamed clips: two decoded QOA frames.
    std::vector<float> blocks[2];
    uint32_t blockIndex[2] = { UINT32_MAX, UINT32_MAX };
    uint32_t blockFrames[2] = {};
    int nextBlockSlot = 0;
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
    // Keeps playing while the system is paused (menu sounds). Ui bus.
    AudioVoiceId PlayUi(const char* soundFile, float volume = 1.0f);
    // One music track at a time: starting another crossfades over fadeSeconds.
    // Music keeps playing while paused. QOA files are streamed.
    AudioVoiceId PlayMusic(const char* soundFile, float volume = 1.0f, bool loop = true, float fadeSeconds = 1.0f);
    void StopMusic(float fadeSeconds = 1.0f);
    AudioVoiceId GetMusicVoice() const { return musicVoice; }

    void StopVoice(AudioVoiceId id, float fadeSeconds = 0.0f);
    bool IsVoicePlaying(AudioVoiceId id) const;
    void UpdateVoice3D(AudioVoiceId id, const Vector3& location, float attenuation);
    void SetVoicePitch(AudioVoiceId id, float pitch);
    void SetVoiceVolume(AudioVoiceId id, float volume);
    // Ramps the voice to `gain` (0..1, on top of its volume) over `seconds`.
    void FadeVoice(AudioVoiceId id, float gain, float seconds);

    // 0..1 (higher values amplify).
    void SetMasterVolume(float volume);
    float GetMasterVolume() const { return masterVolume; }
    void SetBusVolume(AudioBus bus, float volume);
    float GetBusVolume(AudioBus bus) const;

    // Holds every voice that doesn't ignore pause (PlayUi, music); voices keep
    // their positions.
    void SetPaused(bool paused);
    bool IsPaused() const { return paused; }
    void StopAll();
    // False when there is no output device; the system then runs silent.
    bool HasOutput() const { return stream != nullptr; }
    size_t GetVoiceCount() const;

    // Voices that exist at once. When full, a new sound replaces the quietest
    // non-looping one, if that is quieter than the new sound.
    static constexpr size_t kMaxVoices = 256;
    // Voices mixed per buffer: the loudest ones; the rest keep their place silently.
    static constexpr size_t kMaxMixedVoices = 64;
    // QOA clips at least this long are decoded while playing instead of on load.
    static constexpr float kStreamMinSeconds = 8.0f;

private:
    static void SDLAudioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
    AudioVoiceId AddVoice(AudioVoice voice);
    void MixAudio(float* outSamples, int frameCount);
    void ComputeVoiceGains(const AudioVoice& voice, float outGains[2]) const;
    void MixVoice(AudioVoice& voice, const float targetGains[2], float* outSamples, int frameCount);
    void AdvanceVoice(AudioVoice& voice, double frames, float seconds);
    bool ReadFrame(AudioVoice& voice, int frame, float& left, float& right);
    const AudioClip* GetOrLoadClip(const char* soundFile, bool forceStream = false);
    AudioVoice* FindVoice(AudioVoiceId id);
    const AudioVoice* FindVoice(AudioVoiceId id) const;
    void CleanupVoices();

private:
    SDL_AudioStream* stream = nullptr;   // bound to the default playback device
    SDL_AudioSpec deviceSpec = {};       // mixer format: F32 stereo 48 kHz
    std::vector<float> mixBuffer;        // audio thread scratch
    std::vector<size_t> mixOrder;        // audio thread scratch
    std::vector<float> mixGains;         // audio thread scratch, 2 per voice
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

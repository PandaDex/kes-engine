#include "audio_system.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "file_paths.h"
#include "logger.h"
#include "sound_file.h"
#include "profiler.h"

float ComputeStereoPan(const AudioListener& listener, const Vector3& soundPosition)
{
    const Vector3 toSound = soundPosition - listener.position;
    const float distance = Vector3Length(toSound);
    if (distance <= 0.0001f)
        return 0.0f;
    // up x forward: the engine is left-handed.
    const Vector3 right = Vector3Normalize(Vector3Cross(listener.up, listener.forward));
    return ClampFloat(Vector3Dot(toSound / distance, right), -1.0f, 1.0f);
}

bool AudioSystem::Init()
{
    KES_ZONE_FN(AUDIO);
    EngineLog("Audio system initialization begin.");
    mutex = SDL_CreateMutex();
    if (!mutex)
    {
        EngineLogf("SDL_CreateMutex failed: %s", SDL_GetError());
        return false;
    }

    // The mixer works in one fixed format; SDL converts to whatever the
    // hardware wants. Clips are converted to this format when loaded.
    deviceSpec = {SDL_AUDIO_F32, 2, 48000};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec, &AudioSystem::SDLAudioCallback, this);
    if (!stream)
    {
        // No audio device (headless machine, no sound card, driver error):
        // run silent instead of failing startup. Play calls return 0.
        EngineLogf("No audio output (%s); continuing without sound.", SDL_GetError());
        return true;
    }

    if (!SDL_ResumeAudioStreamDevice(stream))
        EngineLogf("SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
    EngineLogf("Audio system initialized. freq=%d channels=%d.", deviceSpec.freq, deviceSpec.channels);
    return true;
}

void AudioSystem::Shutdown()
{
    KES_ZONE_FN(AUDIO);
    EngineLog("Audio system shutdown begin.");
    if (stream)
    {
        // Closes the device; the callback is not called again afterwards.
        SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
    paused = false;

    clipCache.clear();
    voices.clear();

    if (mutex)
    {
        SDL_DestroyMutex(mutex);
        mutex = nullptr;
    }
    EngineLog("Audio system shutdown complete.");
}

void AudioSystem::Update(float dt)
{
    KES_ZONE_FN(AUDIO);
    (void)dt;
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    CleanupVoices();
    SDL_UnlockMutex(mutex);
}

void AudioSystem::SetListener(const AudioListener& newListener)
{
    if (!mutex)
    {
        listener = newListener;
        return;
    }

    SDL_LockMutex(mutex);
    listener = newListener;
    SDL_UnlockMutex(mutex);
}

void AudioSystem::SetPaused(bool pause)
{
    if (!mutex)
    {
        paused = pause;
        return;
    }
    SDL_LockMutex(mutex);
    paused = pause;
    SDL_UnlockMutex(mutex);
}

void AudioSystem::StopAll()
{
    if (!mutex)
        return;
    SDL_LockMutex(mutex);
    voices.clear();
    SDL_UnlockMutex(mutex);
}

bool AudioSystem::AddVoice(const AudioVoice& voice)
{
    SDL_LockMutex(mutex);
    CleanupVoices();
    const bool added = voices.size() < kMaxVoices;
    if (added)
        voices.push_back(voice);
    SDL_UnlockMutex(mutex);
    if (!added)
        EngineLogf("Audio voice limit (%zu) reached; sound skipped.", kMaxVoices);
    return added;
}

bool AudioSystem::LoadClip(const char* soundFile)
{
    return GetOrLoadClip(soundFile) != nullptr;
}

AudioVoiceId AudioSystem::Play2D(const char* soundFile, float volume, float pitch, bool loop)
{
    if (!stream || !mutex)
        return 0;
    const AudioClip* clip = GetOrLoadClip(soundFile);
    if (!clip)
        return 0;

    AudioVoice voice = {};
    voice.id = nextVoiceId++;
    voice.clip = clip;
    voice.volume = volume;
    voice.pitch = pitch;
    voice.loop = loop;
    voice.playing = true;
    return AddVoice(voice) ? voice.id : 0;
}

AudioVoiceId AudioSystem::Play3D(const Vector3& location, float attenuation, const char* soundFile, float volume, float pitch, bool loop)
{
    if (!stream || !mutex)
        return 0;
    const AudioClip* clip = GetOrLoadClip(soundFile);
    if (!clip)
        return 0;

    AudioVoice voice = {};
    voice.id = nextVoiceId++;
    voice.clip = clip;
    voice.volume = volume;
    voice.pitch = pitch;
    voice.loop = loop;
    voice.playing = true;
    voice.spatial = true;
    voice.attenuation = attenuation;
    voice.position = location;
    return AddVoice(voice) ? voice.id : 0;
}

AudioVoiceId AudioSystem::PlayUi(const char* soundFile, float volume)
{
    if (!stream || !mutex)
        return 0;
    const AudioClip* clip = GetOrLoadClip(soundFile);
    if (!clip)
        return 0;

    AudioVoice voice = {};
    voice.id = nextVoiceId++;
    voice.clip = clip;
    voice.volume = volume;
    voice.playing = true;
    voice.ignoresPause = true;
    return AddVoice(voice) ? voice.id : 0;
}

void AudioSystem::StopVoice(AudioVoiceId id)
{
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    AudioVoice* voice = FindVoice(id);
    if (voice)
        voice->playing = false;
    SDL_UnlockMutex(mutex);
}

bool AudioSystem::IsVoicePlaying(AudioVoiceId id) const
{
    if (!mutex)
        return false;

    SDL_LockMutex(mutex);
    const AudioVoice* voice = FindVoice(id);
    const bool isPlaying = voice ? voice->playing : false;
    SDL_UnlockMutex(mutex);
    return isPlaying;
}

void AudioSystem::UpdateVoice3D(AudioVoiceId id, const Vector3& location, float attenuation)
{
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    AudioVoice* voice = FindVoice(id);
    if (voice)
    {
        voice->position = location;
        voice->attenuation = attenuation;
        voice->spatial = true;
    }
    SDL_UnlockMutex(mutex);
}

void AudioSystem::SetVoicePitch(AudioVoiceId id, float pitch)
{
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    AudioVoice* voice = FindVoice(id);
    if (voice)
        voice->pitch = pitch;
    SDL_UnlockMutex(mutex);
}

void AudioSystem::SetVoiceVolume(AudioVoiceId id, float volume)
{
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    AudioVoice* voice = FindVoice(id);
    if (voice)
        voice->volume = volume;
    SDL_UnlockMutex(mutex);
}

// Runs on SDL's audio thread whenever the device needs more data.
void AudioSystem::SDLAudioCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount)
{
    (void)totalAmount;
    AudioSystem* self = static_cast<AudioSystem*>(userdata);
    if (!self || additionalAmount <= 0)
        return;

    const int frameBytes = static_cast<int>(sizeof(float)) * 2;
    const int frameCount = (additionalAmount + frameBytes - 1) / frameBytes;
    std::vector<float>& buffer = self->mixBuffer;   // only touched on this thread
    buffer.assign(static_cast<size_t>(frameCount) * 2, 0.0f);

    SDL_LockMutex(self->mutex);
    self->MixAudio(buffer.data(), frameCount);
    SDL_UnlockMutex(self->mutex);

    SDL_PutAudioStreamData(stream, buffer.data(), frameCount * frameBytes);
}

void AudioSystem::MixAudio(float* outSamples, int frameCount)
{
    KES_ZONE_FN(AUDIO);
    if (!outSamples || frameCount <= 0)
        return;

    for (AudioVoice& voice : voices)
    {
        if (!voice.playing || !voice.clip || voice.clip->frameCount <= 0)
            continue;
        if (paused && !voice.ignoresPause)
            continue;

        float leftGain = voice.volume;
        float rightGain = voice.volume;

        if (voice.spatial)
        {
            const float distance = Vector3Length(voice.position - listener.position);

            // Avoid std::max() because Windows headers may define max as a macro.
            const float attenuation =
                voice.attenuation > 0.001f
                    ? voice.attenuation
                    : 0.001f;

            const float att =
                1.0f / (1.0f + distance * attenuation);

            const float pan = ComputeStereoPan(listener, voice.position);

            leftGain *=
                att * (pan <= 0.0f ? 1.0f : 1.0f - pan);

            rightGain *=
                att * (pan >= 0.0f ? 1.0f : 1.0f + pan);
        }

        for (int frame = 0; frame < frameCount; ++frame)
        {
            int sampleIndex = static_cast<int>(voice.samplePosition);

            if (sampleIndex >= voice.clip->frameCount)
            {
                if (voice.loop)
                {
                    voice.samplePosition = std::fmod(voice.samplePosition, static_cast<double>(voice.clip->frameCount));
                    sampleIndex = static_cast<int>(voice.samplePosition);
                }
                else
                {
                    voice.playing = false;
                    break;
                }
            }

            const int base =
                sampleIndex * voice.clip->channels;

            float left = 0.0f;
            float right = 0.0f;

            if (voice.clip->channels == 1)
            {
                left = voice.clip->samples[static_cast<size_t>(base)];
                right = left;
            }
            else
            {
                left =
                    voice.clip->samples[
                        static_cast<size_t>(base) + 0
                    ];

                right =
                    voice.clip->samples[
                        static_cast<size_t>(base) + 1
                    ];
            }

            outSamples[static_cast<size_t>(frame) * 2 + 0] +=
                left * leftGain;

            outSamples[static_cast<size_t>(frame) * 2 + 1] +=
                right * rightGain;

            voice.samplePosition += voice.pitch > 0.01f ? voice.pitch : 0.01f;
        }
    }

    const int sampleCount = frameCount * 2;

    for (int i = 0; i < sampleCount; ++i)
    {
        outSamples[i] =
            ClampFloat(outSamples[i], -1.0f, 1.0f);
    }
}

const AudioClip* AudioSystem::GetOrLoadClip(const char* soundFile)
{
    KES_ZONE_FN(AUDIO);
    KES_ZONE_TEXTF(AUDIO, "%s", soundFile ? soundFile : "<null>");
    if (!soundFile || !soundFile[0])
        return nullptr;

    const std::string resolvedPath = ResolveAssetPath(soundFile);

    auto it = clipCache.find(resolvedPath);
    if (it != clipCache.end())
        return &it->second;

    SoundFileData sound = {};
    std::string error;
    if (!LoadSoundFile(resolvedPath.c_str(), sound, &error))
    {
        EngineLogf("LoadSoundFile failed for %s: %s", resolvedPath.c_str(), error.c_str());
        return nullptr;
    }

    if (sound.channels == 0 || sound.channels > 2 || sound.sampleRate == 0)
    {
        EngineLogf("Invalid sound format for %s", resolvedPath.c_str());
        return nullptr;
    }

    SDL_AudioSpec srcSpec = {};
    srcSpec.format   = SDL_AUDIO_F32;
    srcSpec.channels = sound.channels;
    srcSpec.freq     = sound.sampleRate;

    AudioClip clip = {};

    const bool needsConversion =
        srcSpec.format != deviceSpec.format ||
        srcSpec.channels != deviceSpec.channels ||
        srcSpec.freq != deviceSpec.freq;

    if (needsConversion)
    {
        Uint8* convertedData = nullptr;
        int convertedLen = 0;
        const int srcLen = static_cast<int>(sound.samples.size() * sizeof(float));

        if (!SDL_ConvertAudioSamples(&srcSpec, reinterpret_cast<const Uint8*>(sound.samples.data()), srcLen,
                                      &deviceSpec, &convertedData, &convertedLen))
        {
            EngineLogf("SDL_ConvertAudioSamples failed for %s: %s", resolvedPath.c_str(), SDL_GetError());
            return nullptr;
        }

        const int floatCount = convertedLen / (int)sizeof(float);
        clip.samples.resize(static_cast<size_t>(floatCount));
        std::memcpy(clip.samples.data(), convertedData, static_cast<size_t>(convertedLen));
        SDL_free(convertedData);
    }
    else
    {
        clip.samples = std::move(sound.samples);
    }

    clip.channels = deviceSpec.channels;
    clip.frameCount = clip.channels > 0 ? static_cast<int>(clip.samples.size() / static_cast<size_t>(clip.channels)) : 0;
    clip.spec = deviceSpec;

    auto inserted = clipCache.emplace(resolvedPath, std::move(clip));
    EngineLogf("Loaded audio clip: %s (%d frames, %d channels).", resolvedPath.c_str(), inserted.first->second.frameCount, inserted.first->second.channels);
    return &inserted.first->second;
}

AudioVoice* AudioSystem::FindVoice(AudioVoiceId id)
{
    for (AudioVoice& voice : voices)
    {
        if (voice.id == id)
            return &voice;
    }
    return nullptr;
}

const AudioVoice* AudioSystem::FindVoice(AudioVoiceId id) const
{
    for (const AudioVoice& voice : voices)
    {
        if (voice.id == id)
            return &voice;
    }
    return nullptr;
}

void AudioSystem::CleanupVoices()
{
    voices.erase(
        std::remove_if(
            voices.begin(),
            voices.end(),
            [](const AudioVoice& voice)
            {
                return !voice.playing;
            }),
        voices.end());
}

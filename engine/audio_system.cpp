#include "audio_system.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "file_paths.h"
#include "logger.h"
#include "sound_file.h"
#include "profiler.h"

namespace
{
    constexpr float kSilentGain = 0.00001f;
    constexpr float kDistanceCutoff = 0.01f; // -40 dB
}

const char* AudioBusName(AudioBus bus)
{
    switch (bus)
    {
    case AudioBus::Effects: return "Effects";
    case AudioBus::Music:   return "Music";
    case AudioBus::Ambient: return "Ambient";
    case AudioBus::Voice:   return "Voice";
    case AudioBus::Ui:      return "Ui";
    case AudioBus::Count:   break;
    }
    return "?";
}

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

float ComputeDistanceGain(float distance, float attenuation)
{
    const float gain = 1.0f / (1.0f + (std::max)(distance, 0.0f) * (std::max)(attenuation, 0.001f));
    return gain <= kDistanceCutoff ? 0.0f : (gain - kDistanceCutoff) / (1.0f - kDistanceCutoff);
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

    // Reserved up front so the audio thread never allocates.
    voices.reserve(kMaxVoices);
    mixOrder.reserve(kMaxVoices);
    mixGains.reserve(kMaxVoices * 2);

    // The mixer works in one fixed format; SDL converts to whatever the
    // hardware wants. Resident clips are converted to this format when loaded.
    deviceSpec = { SDL_AUDIO_F32, 2, 48000 };
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
    musicVoice = 0;

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
    if (musicVoice != 0 && !FindVoice(musicVoice))
        musicVoice = 0;
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
    musicVoice = 0;
    SDL_UnlockMutex(mutex);
}

size_t AudioSystem::GetVoiceCount() const
{
    if (!mutex)
        return 0;
    SDL_LockMutex(mutex);
    const size_t count = static_cast<size_t>(std::count_if(voices.begin(), voices.end(), [](const AudioVoice& voice)
        { return voice.playing; }));
    SDL_UnlockMutex(mutex);
    return count;
}

void AudioSystem::SetMasterVolume(float volume)
{
    if (mutex)
        SDL_LockMutex(mutex);
    masterVolume = (std::max)(volume, 0.0f);
    if (mutex)
        SDL_UnlockMutex(mutex);
}

void AudioSystem::SetBusVolume(AudioBus bus, float volume)
{
    if (bus >= AudioBus::Count)
        return;
    if (mutex)
        SDL_LockMutex(mutex);
    busVolumes[static_cast<size_t>(bus)] = (std::max)(volume, 0.0f);
    if (mutex)
        SDL_UnlockMutex(mutex);
}

float AudioSystem::GetBusVolume(AudioBus bus) const
{
    return bus < AudioBus::Count ? busVolumes[static_cast<size_t>(bus)] : 0.0f;
}

AudioVoiceId AudioSystem::AddVoice(AudioVoice voice)
{
    const AudioVoiceId id = nextVoiceId++;
    if (nextVoiceId == 0)
        nextVoiceId = 1;
    voice.id = id;
    voice.playing = true;
    if (voice.clip && voice.clip->streamed)
    {
        for (std::vector<float>& block : voice.blocks)
            block.resize(static_cast<size_t>(kQoaFrameLength) * static_cast<size_t>(voice.clip->channels));
    }

    SDL_LockMutex(mutex);
    CleanupVoices();
    // Judged at full volume: a voice fading in shouldn't count as silent.
    const float fadeGain = voice.fadeGain;
    voice.fadeGain = (std::max)(voice.fadeGain, voice.fadeTarget);
    float gains[2] = {};
    ComputeVoiceGains(voice, gains);
    voice.fadeGain = fadeGain;
    voice.loudness = (std::max)(gains[0], gains[1]);
    bool added = true;
    if (voices.size() < kMaxVoices)
    {
        voices.push_back(std::move(voice));
    }
    else
    {
        // Full: replace the quietest voice that isn't looping (ambience, music
        // would never come back), if it is quieter than the new sound.
        AudioVoice* quietest = nullptr;
        for (AudioVoice& existing : voices)
        {
            if (!existing.loop && (!quietest || existing.loudness < quietest->loudness))
                quietest = &existing;
        }
        added = quietest && quietest->loudness < voice.loudness;
        if (added)
            *quietest = std::move(voice);
    }
    SDL_UnlockMutex(mutex);
    if (!added)
        EngineLogf("Audio voice limit (%zu) reached; sound skipped.", kMaxVoices);
    return added ? id : 0;
}

bool AudioSystem::LoadClip(const char* soundFile)
{
    return GetOrLoadClip(soundFile) != nullptr;
}

AudioVoiceId AudioSystem::Play2D(const char* soundFile, float volume, float pitch, bool loop, AudioBus bus)
{
    if (!stream || !mutex)
        return 0;
    const AudioClip* clip = GetOrLoadClip(soundFile);
    if (!clip)
        return 0;

    AudioVoice voice = {};
    voice.clip = clip;
    voice.volume = volume;
    voice.pitch = pitch;
    voice.loop = loop;
    voice.bus = bus;
    voice.ignoresPause = bus == AudioBus::Ui;
    return AddVoice(std::move(voice));
}

AudioVoiceId AudioSystem::Play3D(const Vector3& location, float attenuation, const char* soundFile, float volume, float pitch,
    bool loop, AudioBus bus)
{
    if (!stream || !mutex)
        return 0;
    const AudioClip* clip = GetOrLoadClip(soundFile);
    if (!clip)
        return 0;

    AudioVoice voice = {};
    voice.clip = clip;
    voice.volume = volume;
    voice.pitch = pitch;
    voice.loop = loop;
    voice.spatial = true;
    voice.attenuation = attenuation;
    voice.position = location;
    voice.bus = bus;
    voice.ignoresPause = bus == AudioBus::Ui;
    return AddVoice(std::move(voice));
}

AudioVoiceId AudioSystem::PlayUi(const char* soundFile, float volume)
{
    return Play2D(soundFile, volume, 1.0f, false, AudioBus::Ui);
}

AudioVoiceId AudioSystem::PlayMusic(const char* soundFile, float volume, bool loop, float fadeSeconds)
{
    if (!stream || !mutex)
        return 0;
    const AudioClip* clip = GetOrLoadClip(soundFile, true);
    if (!clip)
        return 0;

    StopMusic(fadeSeconds);

    AudioVoice voice = {};
    voice.clip = clip;
    voice.volume = volume;
    voice.loop = loop;
    voice.bus = AudioBus::Music;
    voice.ignoresPause = true;
    if (fadeSeconds > 0.0f)
    {
        voice.fadeGain = 0.0f;
        voice.fadeRate = 1.0f / fadeSeconds;
    }
    const AudioVoiceId id = AddVoice(std::move(voice));
    SDL_LockMutex(mutex);
    musicVoice = id;
    SDL_UnlockMutex(mutex);
    return id;
}

void AudioSystem::StopMusic(float fadeSeconds)
{
    if (!mutex)
        return;
    SDL_LockMutex(mutex);
    const AudioVoiceId id = musicVoice;
    musicVoice = 0;
    SDL_UnlockMutex(mutex);
    if (id != 0)
        StopVoice(id, fadeSeconds);
}

void AudioSystem::StopVoice(AudioVoiceId id, float fadeSeconds)
{
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    if (AudioVoice* voice = FindVoice(id))
    {
        if (fadeSeconds <= 0.0f || voice->fadeGain <= 0.0f)
        {
            voice->playing = false;
        }
        else
        {
            voice->fadeTarget = 0.0f;
            voice->fadeRate = voice->fadeGain / fadeSeconds;
            voice->stopAfterFade = true;
        }
    }
    SDL_UnlockMutex(mutex);
}

void AudioSystem::FadeVoice(AudioVoiceId id, float gain, float seconds)
{
    if (!mutex)
        return;

    SDL_LockMutex(mutex);
    if (AudioVoice* voice = FindVoice(id))
    {
        voice->fadeTarget = (std::max)(gain, 0.0f);
        voice->stopAfterFade = false;
        if (seconds <= 0.0f)
            voice->fadeGain = voice->fadeTarget;
        else
            voice->fadeRate = std::fabs(voice->fadeTarget - voice->fadeGain) / seconds;
    }
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
    std::vector<float>& buffer = self->mixBuffer; // only touched on this thread
    buffer.assign(static_cast<size_t>(frameCount) * 2, 0.0f);

    SDL_LockMutex(self->mutex);
    self->MixAudio(buffer.data(), frameCount);
    SDL_UnlockMutex(self->mutex);

    SDL_PutAudioStreamData(stream, buffer.data(), frameCount * frameBytes);
}

void AudioSystem::ComputeVoiceGains(const AudioVoice& voice, float outGains[2]) const
{
    const float bus = voice.bus < AudioBus::Count ? busVolumes[static_cast<size_t>(voice.bus)] : 1.0f;
    const float base = (std::max)(voice.volume, 0.0f) * voice.fadeGain * masterVolume * bus;
    outGains[0] = base;
    outGains[1] = base;
    if (!voice.spatial)
        return;

    const float distanceGain = ComputeDistanceGain(Vector3Length(voice.position - listener.position), voice.attenuation);
    const float pan = ComputeStereoPan(listener, voice.position);
    outGains[0] *= distanceGain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
    outGains[1] *= distanceGain * (pan >= 0.0f ? 1.0f : 1.0f + pan);
}

// Frame `frame` of the voice's clip. Streamed clips decode QOA frames into two
// slots, so reading across a frame boundary doesn't decode the same one twice.
bool AudioSystem::ReadFrame(AudioVoice& voice, int frame, float& left, float& right)
{
    const AudioClip& clip = *voice.clip;
    const float* samples = nullptr;
    if (!clip.streamed)
    {
        samples = clip.samples.data() + static_cast<size_t>(frame) * static_cast<size_t>(clip.channels);
    }
    else
    {
        const uint32_t block = static_cast<uint32_t>(frame) / kQoaFrameLength;
        const uint32_t offset = static_cast<uint32_t>(frame) % kQoaFrameLength;
        int slot = voice.blockIndex[0] == block ? 0 : voice.blockIndex[1] == block ? 1
                                                                                   : -1;
        if (slot < 0)
        {
            slot = 1 - voice.nextBlockSlot;
            voice.blockFrames[slot] = DecodeQoaFrame(clip.encoded, static_cast<uint32_t>(clip.channels), block, voice.blocks[slot].data());
            voice.blockIndex[slot] = voice.blockFrames[slot] > 0 ? block : UINT32_MAX;
        }
        voice.nextBlockSlot = slot;
        if (offset >= voice.blockFrames[slot])
            return false;
        samples = voice.blocks[slot].data() + static_cast<size_t>(offset) * static_cast<size_t>(clip.channels);
    }
    left = samples[0];
    right = clip.channels > 1 ? samples[1] : left;
    return true;
}

void AudioSystem::AdvanceVoice(AudioVoice& voice, double frames, float seconds)
{
    (void)seconds;
    voice.samplePosition += frames;
    const double length = static_cast<double>(voice.clip->frameCount);
    if (voice.samplePosition >= length)
    {
        if (voice.loop)
            voice.samplePosition = std::fmod(voice.samplePosition, length);
        else
            voice.playing = false;
    }
}

void AudioSystem::MixVoice(AudioVoice& voice, const float targetGains[2], float* outSamples, int frameCount)
{
    const AudioClip& clip = *voice.clip;
    const double step = (std::max)(voice.pitch, 0.01f) * static_cast<double>(clip.sampleRate) / deviceSpec.freq;
    // Gains ramp across the buffer: a moving 3D sound or a volume change
    // would otherwise jump between buffers and click.
    const float startLeft = voice.gainApplied ? voice.appliedGain[0] : targetGains[0];
    const float startRight = voice.gainApplied ? voice.appliedGain[1] : targetGains[1];
    const float inverseCount = 1.0f / static_cast<float>(frameCount);

    for (int frame = 0; frame < frameCount; ++frame)
    {
        int index = static_cast<int>(voice.samplePosition);
        if (index >= clip.frameCount)
        {
            if (!voice.loop)
            {
                voice.playing = false;
                break;
            }
            voice.samplePosition = std::fmod(voice.samplePosition, static_cast<double>(clip.frameCount));
            index = static_cast<int>(voice.samplePosition);
        }
        int next = index + 1;
        if (next >= clip.frameCount)
            next = voice.loop ? 0 : index;

        float l0 = 0.0f, r0 = 0.0f, l1 = 0.0f, r1 = 0.0f;
        if (!ReadFrame(voice, index, l0, r0) || !ReadFrame(voice, next, l1, r1))
        {
            voice.playing = false;
            break;
        }
        const float t = static_cast<float>(voice.samplePosition - index);
        const float ramp = static_cast<float>(frame + 1) * inverseCount;
        const float leftGain = startLeft + (targetGains[0] - startLeft) * ramp;
        const float rightGain = startRight + (targetGains[1] - startRight) * ramp;
        outSamples[static_cast<size_t>(frame) * 2 + 0] += (l0 + (l1 - l0) * t) * leftGain;
        outSamples[static_cast<size_t>(frame) * 2 + 1] += (r0 + (r1 - r0) * t) * rightGain;
        voice.samplePosition += step;
    }

    voice.appliedGain[0] = targetGains[0];
    voice.appliedGain[1] = targetGains[1];
    voice.gainApplied = true;
}

void AudioSystem::MixAudio(float* outSamples, int frameCount)
{
    KES_ZONE_FN(AUDIO);
    if (!outSamples || frameCount <= 0)
        return;

    const float seconds = static_cast<float>(frameCount) / static_cast<float>(deviceSpec.freq);
    mixOrder.clear();
    mixGains.resize(voices.size() * 2);

    for (size_t i = 0; i < voices.size(); ++i)
    {
        AudioVoice& voice = voices[i];
        if (!voice.playing || !voice.clip || voice.clip->frameCount <= 0)
            continue;
        if (paused && !voice.ignoresPause)
            continue;

        if (voice.fadeGain != voice.fadeTarget)
        {
            const float change = voice.fadeRate * seconds;
            voice.fadeGain = voice.fadeGain < voice.fadeTarget ? (std::min)(voice.fadeGain + change, voice.fadeTarget)
                                                               : (std::max)(voice.fadeGain - change, voice.fadeTarget);
        }

        float* gains = &mixGains[i * 2];
        ComputeVoiceGains(voice, gains);
        voice.loudness = (std::max)(gains[0], gains[1]);
        const bool wasAudible = voice.gainApplied && (std::max)(voice.appliedGain[0], voice.appliedGain[1]) > kSilentGain;
        if (voice.loudness > kSilentGain || wasAudible) // a voice going silent still ramps down
            mixOrder.push_back(i);
        else
        {
            const double step = (std::max)(voice.pitch, 0.01f) * static_cast<double>(voice.clip->sampleRate) / deviceSpec.freq;
            AdvanceVoice(voice, step * frameCount, seconds);
            voice.gainApplied = false;
        }

        if (voice.stopAfterFade && voice.fadeGain <= 0.0f && !wasAudible)
            voice.playing = false;
    }

    // Only the loudest voices are mixed; the rest keep their place in the clip.
    if (mixOrder.size() > kMaxMixedVoices)
    {
        std::nth_element(mixOrder.begin(), mixOrder.begin() + kMaxMixedVoices, mixOrder.end(),
            [this](size_t a, size_t b)
            { return voices[a].loudness > voices[b].loudness; });
        for (size_t i = kMaxMixedVoices; i < mixOrder.size(); ++i)
        {
            AudioVoice& voice = voices[mixOrder[i]];
            const double step = (std::max)(voice.pitch, 0.01f) * static_cast<double>(voice.clip->sampleRate) / deviceSpec.freq;
            AdvanceVoice(voice, step * frameCount, seconds);
            voice.gainApplied = false;
        }
        mixOrder.resize(kMaxMixedVoices);
    }
    for (size_t index : mixOrder)
    {
        AudioVoice& voice = voices[index];
        MixVoice(voice, &mixGains[index * 2], outSamples, frameCount);
        if (voice.stopAfterFade && voice.fadeGain <= 0.0f)
            voice.playing = false;
    }
    KES_ZONE_VALUE(AUDIO, static_cast<int64_t>(mixOrder.size()));

    const int sampleCount = frameCount * 2;
    for (int i = 0; i < sampleCount; ++i)
        outSamples[i] = ClampFloat(outSamples[i], -1.0f, 1.0f);
}

const AudioClip* AudioSystem::GetOrLoadClip(const char* soundFile, bool forceStream)
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

    AudioClip clip = {};
    const float seconds = static_cast<float>(sound.frameCount) / static_cast<float>(sound.sampleRate);
    if (sound.encoding == SoundEncoding::Qoa && (forceStream || seconds >= kStreamMinSeconds))
    {
        clip.streamed = true;
        clip.channels = static_cast<int>(sound.channels);
        clip.sampleRate = static_cast<int>(sound.sampleRate);
        clip.frameCount = static_cast<int>(sound.frameCount);
        clip.encoded = std::move(sound.encoded);
        auto inserted = clipCache.emplace(resolvedPath, std::move(clip));
        EngineLogf("Loaded audio clip: %s (%.1f s, %u channels, streamed, %zu KB).", resolvedPath.c_str(), seconds, sound.channels,
            inserted.first->second.encoded.size() / 1024);
        return &inserted.first->second;
    }

    if (!DecodeSoundSamples(sound, &error))
    {
        EngineLogf("Decoding %s failed: %s", resolvedPath.c_str(), error.c_str());
        return nullptr;
    }

    SDL_AudioSpec srcSpec = {};
    srcSpec.format = SDL_AUDIO_F32;
    srcSpec.channels = static_cast<int>(sound.channels);
    srcSpec.freq = static_cast<int>(sound.sampleRate);

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
    clip.sampleRate = deviceSpec.freq;
    clip.frameCount = clip.channels > 0 ? static_cast<int>(clip.samples.size() / static_cast<size_t>(clip.channels)) : 0;

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

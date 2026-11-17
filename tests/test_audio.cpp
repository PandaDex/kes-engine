#include "test_framework.h"
#include <cmath>
#include <cstring>
#include <functional>
#include "audio_system.h"
#include "sound_file.h"

// These run the real mixer through SDL's disk audio driver, which writes what
// the device would play to a file, so no sound card or listening is needed.
namespace
{
    struct ChannelEnergy
    {
        double left = 0.0;
        double right = 0.0;
    };

    std::string WriteTone(const kestest::TempDir& dir)
    {
        SoundFileData tone;
        tone.sampleRate = 48000;
        tone.channels = 1;
        tone.frameCount = 12000;
        for (uint32_t frame = 0; frame < tone.frameCount; ++frame)
            tone.samples.push_back(0.5f * std::sin(static_cast<float>(frame) * 0.0576f));
        const std::string path = dir.File("tone.ksound");
        REQUIRE(SaveKSoundFile(path.c_str(), tone));
        return path;
    }

    ChannelEnergy Capture(const kestest::TempDir& dir, const std::function<void(AudioSystem&)>& play)
    {
        const std::string output = dir.File("output.raw");
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "disk");
        SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, output.c_str());
        REQUIRE(SDL_InitSubSystem(SDL_INIT_AUDIO));

        SDL_AudioSpec spec = {};
        AudioSystem audio;
        REQUIRE(audio.Init());
        REQUIRE(audio.HasOutput());
        REQUIRE(SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr));
        play(audio);
        audio.Shutdown();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
        SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);

        REQUIRE(spec.channels == 2);
        const std::vector<uint8_t> bytes = kestest::ReadBytes(output);
        ChannelEnergy energy;
        const size_t sampleSize = SDL_AUDIO_BYTESIZE(spec.format);
        for (size_t offset = 0; offset + sampleSize * 2 <= bytes.size(); offset += sampleSize * 2)
        {
            double channels[2] = {};
            for (int channel = 0; channel < 2; ++channel)
            {
                const uint8_t* sample = bytes.data() + offset + static_cast<size_t>(channel) * sampleSize;
                if (spec.format == SDL_AUDIO_F32)
                {
                    float value = 0.0f;
                    std::memcpy(&value, sample, sizeof(value));
                    channels[channel] = value;
                }
                else
                {
                    REQUIRE(spec.format == SDL_AUDIO_S16);
                    int16_t value = 0;
                    std::memcpy(&value, sample, sizeof(value));
                    channels[channel] = value / 32768.0;
                }
            }
            energy.left += channels[0] * channels[0];
            energy.right += channels[1] * channels[1];
        }
        return energy;
    }
}

TEST_CASE("audio: a 2D sound reaches both channels")
{
    kestest::TempDir dir;
    const std::string tone = WriteTone(dir);
    const ChannelEnergy energy = Capture(dir, [&](AudioSystem& audio)
    {
        CHECK(audio.Play2D(tone.c_str()) != 0);
        SDL_Delay(500);
    });
    CHECK(energy.left > 1.0);
    CHECK_NEAR(energy.left / energy.right, 1.0, 0.01);
}

TEST_CASE("audio: a sound to the right plays in the right channel")
{
    kestest::TempDir dir;
    const std::string tone = WriteTone(dir);
    const auto play = [&](const Vector3& position)
    {
        return Capture(dir, [&](AudioSystem& audio)
        {
            CHECK(audio.Play3D(position, 0.05f, tone.c_str()) != 0);
            SDL_Delay(500);
        });
    };

    const ChannelEnergy right = play({ 4.0f, 0.0f, 0.0f });
    CHECK(right.right > 1.0);
    CHECK(right.left < right.right * 0.01);

    const ChannelEnergy left = play({ -4.0f, 0.0f, 0.0f });
    CHECK(left.left > 1.0);
    CHECK(left.right < left.left * 0.01);
}

TEST_CASE("audio: pausing holds game sounds but not UI sounds")
{
    kestest::TempDir dir;
    const std::string tone = WriteTone(dir);

    const ChannelEnergy held = Capture(dir, [&](AudioSystem& audio)
    {
        audio.SetPaused(true);
        CHECK(audio.Play2D(tone.c_str()) != 0);
        SDL_Delay(400);
    });
    CHECK(held.left < 0.001);

    const ChannelEnergy ui = Capture(dir, [&](AudioSystem& audio)
    {
        audio.SetPaused(true);
        CHECK(audio.PlayUi(tone.c_str()) != 0);
        SDL_Delay(500);
    });
    CHECK(ui.left > 1.0);

    const ChannelEnergy resumed = Capture(dir, [&](AudioSystem& audio)
    {
        audio.SetPaused(true);
        CHECK(audio.Play2D(tone.c_str()) != 0);
        SDL_Delay(100);
        audio.SetPaused(false);
        SDL_Delay(500);
    });
    CHECK(resumed.left > 1.0);
}

TEST_CASE("audio: a sound to the right pans right")
{
    AudioListener listener;   // at the origin, facing +Z
    CHECK_NEAR(ComputeStereoPan(listener, { 5.0f, 0.0f, 0.0f }), 1.0, 1e-5);
    CHECK_NEAR(ComputeStereoPan(listener, { -5.0f, 0.0f, 0.0f }), -1.0, 1e-5);
    CHECK_NEAR(ComputeStereoPan(listener, { 0.0f, 0.0f, 5.0f }), 0.0, 1e-5);
    CHECK_NEAR(ComputeStereoPan(listener, listener.position), 0.0, 1e-5);

    listener.forward = { 1.0f, 0.0f, 0.0f };   // turned right: +Z is now on the left
    CHECK_NEAR(ComputeStereoPan(listener, { 0.0f, 0.0f, 5.0f }), -1.0, 1e-5);
}

TEST_CASE("audio: the sample position keeps advancing on long clips")
{
    AudioVoice voice;
    voice.samplePosition = 16777216.0;   // 2^24 frames, under 6 minutes at 48 kHz
    voice.samplePosition += 1.0f;
    CHECK(voice.samplePosition == 16777217.0);
}

namespace
{
    SoundFileData MakeTone(uint32_t frames, uint32_t channels, uint32_t sampleRate = 48000)
    {
        SoundFileData tone;
        tone.sampleRate = sampleRate;
        tone.channels = channels;
        tone.frameCount = frames;
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            for (uint32_t channel = 0; channel < channels; ++channel)
                tone.samples.push_back(0.5f * std::sin(static_cast<float>(frame) * (0.0576f + 0.01f * channel)));
        }
        return tone;
    }

    std::string WriteQoaTone(const kestest::TempDir& dir, const char* name, uint32_t frames, uint32_t sampleRate = 48000)
    {
        SoundFileData tone = MakeTone(frames, 2, sampleRate);
        tone.encoding = SoundEncoding::Qoa;
        const std::string path = dir.File(name);
        REQUIRE(SaveKSoundFile(path.c_str(), tone));
        return path;
    }
}

TEST_CASE("ksound: QOA round trip stays close to the source")
{
    kestest::TempDir dir;
    const SoundFileData source = MakeTone(12345, 2);   // not a multiple of the QOA frame length
    SoundFileData encoded = source;
    encoded.encoding = SoundEncoding::Qoa;
    const std::string path = dir.File("tone.ksound");
    REQUIRE(SaveKSoundFile(path.c_str(), encoded));

    SoundFileData loaded;
    std::string error;
    REQUIRE(LoadKSoundFile(path.c_str(), loaded, &error));
    CHECK(loaded.encoding == SoundEncoding::Qoa);
    CHECK(loaded.frameCount == source.frameCount);
    CHECK(loaded.encoded.size() < source.samples.size() * sizeof(float) / 8);
    REQUIRE(DecodeSoundSamples(loaded, &error));
    REQUIRE(loaded.samples.size() == source.samples.size());

    double signal = 0.0;
    double noise = 0.0;
    for (size_t i = 0; i < source.samples.size(); ++i)
    {
        signal += source.samples[i] * source.samples[i];
        const double difference = loaded.samples[i] - source.samples[i];
        noise += difference * difference;
    }
    CHECK(10.0 * std::log10(signal / noise) > 30.0);   // dB SNR
}

TEST_CASE("audio: distance gain reaches exactly zero far away")
{
    CHECK_NEAR(ComputeDistanceGain(0.0f, 1.0f), 1.0, 1e-6);
    CHECK(ComputeDistanceGain(10.0f, 1.0f) > 0.05f);
    CHECK(ComputeDistanceGain(98.0f, 1.0f) > 0.0f);
    CHECK(ComputeDistanceGain(100.0f, 1.0f) == 0.0f);
    CHECK(ComputeDistanceGain(5000.0f, 0.05f) == 0.0f);
}

TEST_CASE("audio: bus and master volumes scale the output")
{
    kestest::TempDir dir;
    const std::string tone = WriteTone(dir);
    const auto play = [&](float master, float effects)
    {
        return Capture(dir, [&](AudioSystem& audio)
        {
            audio.SetMasterVolume(master);
            audio.SetBusVolume(AudioBus::Effects, effects);
            CHECK(audio.Play2D(tone.c_str()) != 0);
            SDL_Delay(500);
        });
    };
    const ChannelEnergy full = play(1.0f, 1.0f);
    const ChannelEnergy half = play(0.5f, 1.0f);
    const ChannelEnergy muted = play(1.0f, 0.0f);
    REQUIRE(full.left > 1.0);
    CHECK_NEAR(half.left / full.left, 0.25, 0.02);
    CHECK(muted.left < 0.001);
}

TEST_CASE("audio: QOA music streams and crossfades")
{
    kestest::TempDir dir;
    const std::string first = WriteQoaTone(dir, "music_a.ksound", 48000 * 10);
    const std::string second = WriteQoaTone(dir, "music_b.ksound", 44100 * 10, 44100);   // resampled while playing
    bool stillPlaying = false;
    bool replaced = false;
    const ChannelEnergy energy = Capture(dir, [&](AudioSystem& audio)
    {
        const AudioVoiceId a = audio.PlayMusic(first.c_str(), 1.0f, true, 0.0f);
        CHECK(a != 0);
        SDL_Delay(300);
        const AudioVoiceId b = audio.PlayMusic(second.c_str(), 1.0f, true, 0.1f);
        CHECK(b != 0);
        CHECK(audio.GetMusicVoice() == b);
        SDL_Delay(400);
        replaced = !audio.IsVoicePlaying(a);
        stillPlaying = audio.IsVoicePlaying(b);
    });
    CHECK(energy.left > 1.0);
    CHECK(energy.right > 1.0);
    CHECK(replaced);
    CHECK(stillPlaying);
}

TEST_CASE("audio: a full voice list gives way to louder sounds only")
{
    kestest::TempDir dir;
    const std::string tone = WriteTone(dir);
    Capture(dir, [&](AudioSystem& audio)
    {
        // Distant, so quiet; the mixer has to see them once to know.
        for (size_t i = 0; i < AudioSystem::kMaxVoices; ++i)
            REQUIRE(audio.Play3D({ 40.0f, 0.0f, 0.0f }, 0.05f, tone.c_str(), 0.2f) != 0);
        SDL_Delay(50);
        CHECK(audio.GetVoiceCount() == AudioSystem::kMaxVoices);
        CHECK(audio.Play3D({ 400.0f, 0.0f, 0.0f }, 1.0f, tone.c_str()) == 0);   // silent: rejected
        CHECK(audio.Play2D(tone.c_str()) != 0);                                 // loud: replaces one
        CHECK(audio.GetVoiceCount() == AudioSystem::kMaxVoices);
    });
}

TEST_CASE("audio: a voice stopped with a fade ends after it")
{
    kestest::TempDir dir;
    const std::string tone = WriteQoaTone(dir, "long.ksound", 48000 * 10);
    Capture(dir, [&](AudioSystem& audio)
    {
        const AudioVoiceId id = audio.Play2D(tone.c_str(), 1.0f, 1.0f, true);
        REQUIRE(id != 0);
        SDL_Delay(100);
        audio.StopVoice(id, 0.2f);
        SDL_Delay(60);
        CHECK(audio.IsVoicePlaying(id));
        SDL_Delay(400);
        CHECK(!audio.IsVoicePlaying(id));
    });
}

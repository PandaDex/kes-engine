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

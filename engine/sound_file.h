#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class SoundEncoding : uint32_t
{
    PcmFloat = 0, // interleaved float32
    Qoa = 1, // QOA ("Quite OK Audio"), ~3.2 bits per sample, lossy
};

struct SoundFileData
{
    std::string sourcePath;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t frameCount = 0;
    SoundEncoding encoding = SoundEncoding::PcmFloat;
    std::vector<float> samples; // PcmFloat, or after DecodeSoundSamples
    std::vector<uint8_t> encoded; // Qoa: a complete QOA stream (header + frames)
};

// QOA frames are self-contained: frame i starts at kQoaFileHeaderSize + i * QoaFrameBytes(channels)
// and decodes kQoaFrameLength frames of audio (the last one fewer).
inline constexpr uint32_t kQoaFileHeaderSize = 8;
inline constexpr uint32_t kQoaFrameLength = 256 * 20;
inline constexpr uint32_t QoaFrameBytes(uint32_t channels)
{
    return 8 + 16 * channels + 8 * 256 * channels;
}

// Qoa files stay encoded; DecodeSoundSamples turns them into float samples.
bool LoadSoundFile(const char* path, SoundFileData& outSound, std::string* outError = nullptr);
bool LoadKSoundFile(const char* path, SoundFileData& outSound, std::string* outError = nullptr);
// Writes `encoding`. Qoa is encoded from `samples` when `encoded` is empty.
bool SaveKSoundFile(const char* path, const SoundFileData& sound, std::string* outError = nullptr);

bool EncodeSoundQoa(SoundFileData& sound, std::string* outError = nullptr);
bool DecodeSoundSamples(SoundFileData& sound, std::string* outError = nullptr);
// Decodes QOA frame `frameIndex` of `encoded` into interleaved floats. Returns
// the number of audio frames written (0 on bad data). `out` must hold
// kQoaFrameLength * channels floats.
uint32_t DecodeQoaFrame(const std::vector<uint8_t>& encoded, uint32_t channels, uint32_t frameIndex, float* out);

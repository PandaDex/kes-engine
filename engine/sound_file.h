#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SoundFileData
{
    std::string sourcePath;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t frameCount = 0;
    std::vector<float> samples;
};

bool LoadSoundFile(const char* path, SoundFileData& outSound, std::string* outError = nullptr);
bool LoadKSoundFile(const char* path, SoundFileData& outSound, std::string* outError = nullptr);
bool SaveKSoundFile(const char* path, const SoundFileData& sound, std::string* outError = nullptr);

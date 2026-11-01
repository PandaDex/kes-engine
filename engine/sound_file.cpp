#include "sound_file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "file_paths.h"
#include "pak_file_system.h"

namespace
{
    constexpr uint32_t kKSoundMagic = 0x444E534Bu; // KSND
    constexpr uint32_t kKSoundVersion = 1u;

    template<typename T>
    bool WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return file.good();
    }

    template<typename T>
    bool ReadValue(std::istream& file, T& value)
    {
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
        return file.good();
    }

    bool WriteString(std::ofstream& file, const std::string& value)
    {
        const uint32_t length = static_cast<uint32_t>(value.size());
        if (!WriteValue(file, length))
            return false;

        if (length > 0)
            file.write(value.data(), length);
        return file.good();
    }

    bool ReadString(std::istream& file, std::string& value)
    {
        uint32_t length = 0;
        if (!ReadValue(file, length))
            return false;

        value.resize(length);
        if (length > 0)
            file.read(value.data(), length);
        return file.good();
    }

    std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });
        return value;
    }
}

bool LoadSoundFile(const char* path, SoundFileData& outSound, std::string* outError)
{
    outSound = {};

    const std::string resolvedPath = ResolveAssetPath(path);
    const std::string extension = Lowercase(std::filesystem::path(resolvedPath).extension().string());

    if (extension == ".ksound")
        return LoadKSoundFile(resolvedPath.c_str(), outSound, outError);

    if (outError)
        *outError = "Unsupported sound format: " + extension + ". Import it to .ksound first.";
    return false;
}

bool LoadKSoundFile(const char* path, SoundFileData& outSound, std::string* outError)
{
    outSound = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;

    const std::string fileData(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    std::istringstream file(fileData, std::ios::binary);

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t sampleCount = 0;
    if (!ReadValue(file, magic) ||
        !ReadValue(file, version) ||
        !ReadString(file, outSound.sourcePath) ||
        !ReadValue(file, outSound.sampleRate) ||
        !ReadValue(file, outSound.channels) ||
        !ReadValue(file, outSound.frameCount) ||
        !ReadValue(file, sampleCount))
    {
        if (outError)
            *outError = "Failed to read sound header";
        return false;
    }

    if (magic != kKSoundMagic)
    {
        if (outError)
            *outError = "Invalid sound magic";
        return false;
    }

    if (version != kKSoundVersion)
    {
        if (outError)
            *outError = "Unsupported sound version";
        return false;
    }

    if (outSound.sampleRate == 0 || outSound.channels == 0 || outSound.channels > 2)
    {
        if (outError)
            *outError = "Invalid sound format";
        return false;
    }

    const uint64_t expectedSampleCount = static_cast<uint64_t>(outSound.frameCount) * static_cast<uint64_t>(outSound.channels);
    if (sampleCount != expectedSampleCount)
    {
        if (outError)
            *outError = "Sound sample count does not match frame count";
        return false;
    }

    outSound.samples.resize(sampleCount);
    if (sampleCount > 0)
        file.read(reinterpret_cast<char*>(outSound.samples.data()), static_cast<std::streamsize>(sampleCount * sizeof(float)));
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to read sound samples";
        return false;
    }

    return true;
}

bool SaveKSoundFile(const char* path, const SoundFileData& sound, std::string* outError)
{
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing sound output path";
        return false;
    }

    if (sound.sampleRate == 0 || sound.channels == 0 || sound.channels > 2)
    {
        if (outError)
            *outError = "Only mono and stereo sounds with a valid sample rate are supported";
        return false;
    }

    if (sound.samples.size() % sound.channels != 0)
    {
        if (outError)
            *outError = "Sound sample data is incomplete";
        return false;
    }

    const uint32_t derivedFrameCount = static_cast<uint32_t>(sound.samples.size() / sound.channels);
    if (sound.frameCount != 0 && sound.frameCount != derivedFrameCount)
    {
        if (outError)
            *outError = "Sound frame count does not match sample data";
        return false;
    }

    const std::filesystem::path outputPath(path);
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path());

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open sound output file";
        return false;
    }

    const uint32_t sampleCount = static_cast<uint32_t>(sound.samples.size());
    if (!WriteValue(file, kKSoundMagic) ||
        !WriteValue(file, kKSoundVersion) ||
        !WriteString(file, sound.sourcePath) ||
        !WriteValue(file, sound.sampleRate) ||
        !WriteValue(file, sound.channels) ||
        !WriteValue(file, derivedFrameCount) ||
        !WriteValue(file, sampleCount))
    {
        if (outError)
            *outError = "Failed to write sound header";
        return false;
    }

    if (sampleCount > 0)
        file.write(reinterpret_cast<const char*>(sound.samples.data()), static_cast<std::streamsize>(sound.samples.size() * sizeof(float)));
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write sound samples";
        return false;
    }

    return true;
}

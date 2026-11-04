#include "sound_file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "binary_reader.h"
#include "file_paths.h"
#include "pak_file_system.h"
#include "profiler.h"

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

    bool WriteString(std::ofstream& file, const std::string& value)
    {
        const uint32_t length = static_cast<uint32_t>(value.size());
        if (!WriteValue(file, length))
            return false;

        if (length > 0)
            file.write(value.data(), length);
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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outSound = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;

    const auto fail = [&](const char* message)
    {
        if (outError)
            *outError = message;
        outSound = {};
        return false;
    };

    BinaryReader file(fileBytes);
    const uint32_t magic = file.u32();
    const uint32_t version = file.u32();
    if (!file.ok())
        return fail("Failed to read sound header");
    if (magic != kKSoundMagic)
        return fail("Invalid sound magic");
    if (version != kKSoundVersion)
        return fail("Unsupported sound version");

    outSound.sourcePath = file.string();
    outSound.sampleRate = file.u32();
    outSound.channels = file.u32();
    outSound.frameCount = file.u32();
    const uint32_t sampleCount = file.u32();
    if (!file.ok())
        return fail("Failed to read sound header");

    if (outSound.sampleRate == 0 || outSound.channels == 0 || outSound.channels > 2)
        return fail("Invalid sound format");

    const uint64_t expectedSampleCount = static_cast<uint64_t>(outSound.frameCount) * static_cast<uint64_t>(outSound.channels);
    if (sampleCount != expectedSampleCount)
        return fail("Sound sample count does not match frame count");
    if (!file.count(sampleCount, sizeof(float)))
        return fail("Failed to read sound samples");

    outSound.samples.resize(sampleCount);
    file.bytes(outSound.samples.data(), sampleCount * sizeof(float));
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

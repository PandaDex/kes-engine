#include "sound_file.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "binary_reader.h"
#include "file_paths.h"
#include "pak_file_system.h"
#include "profiler.h"

#include <qoa.h> // implementation in qoa_codec.c

namespace
{
    constexpr uint32_t kKSoundMagic = 0x444E534Bu; // KSND
    constexpr uint32_t kKSoundVersion = 2u; // 1: float PCM only

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
    if (version < 1 || version > kKSoundVersion)
        return fail("Unsupported sound version");

    outSound.sourcePath = file.string();
    outSound.sampleRate = file.u32();
    outSound.channels = file.u32();
    outSound.frameCount = file.u32();
    if (version >= 2)
        outSound.encoding = static_cast<SoundEncoding>(file.u32());
    const uint32_t payloadCount = file.u32(); // PcmFloat: samples, Qoa: bytes
    if (!file.ok())
        return fail("Failed to read sound header");

    if (outSound.sampleRate == 0 || outSound.channels == 0 || outSound.channels > 2)
        return fail("Invalid sound format");

    if (outSound.encoding == SoundEncoding::Qoa)
    {
        if (!file.count(payloadCount, 1))
            return fail("Failed to read sound data");
        outSound.encoded.resize(payloadCount);
        file.bytes(outSound.encoded.data(), payloadCount);
        qoa_desc desc = {};
        if (!qoa_decode_header(outSound.encoded.data(), static_cast<int>(outSound.encoded.size()), &desc) ||
            desc.channels != outSound.channels || desc.samples != outSound.frameCount || desc.samplerate != outSound.sampleRate)
            return fail("Invalid QOA sound data");
        const uint64_t frames = (static_cast<uint64_t>(outSound.frameCount) + kQoaFrameLength - 1) / kQoaFrameLength;
        if (outSound.encoded.size() < kQoaFileHeaderSize + (frames - 1) * QoaFrameBytes(outSound.channels))
            return fail("QOA sound data is truncated");
        return true;
    }
    if (outSound.encoding != SoundEncoding::PcmFloat)
        return fail("Unknown sound encoding");

    const uint64_t expectedSampleCount = static_cast<uint64_t>(outSound.frameCount) * static_cast<uint64_t>(outSound.channels);
    if (payloadCount != expectedSampleCount)
        return fail("Sound sample count does not match frame count");
    if (!file.count(payloadCount, sizeof(float)))
        return fail("Failed to read sound samples");

    outSound.samples.resize(payloadCount);
    file.bytes(outSound.samples.data(), payloadCount * sizeof(float));
    return true;
}

bool EncodeSoundQoa(SoundFileData& sound, std::string* outError)
{
    const auto fail = [&](const char* message)
    {
        if (outError)
            *outError = message;
        return false;
    };
    if (sound.channels == 0 || sound.channels > 2 || sound.sampleRate == 0)
        return fail("Only mono and stereo sounds with a valid sample rate are supported");
    if (sound.samples.empty() || sound.samples.size() % sound.channels != 0)
        return fail("Sound sample data is empty or incomplete");

    std::vector<short> pcm(sound.samples.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<short>(std::lround(std::clamp(sound.samples[i], -1.0f, 1.0f) * 32767.0f));

    qoa_desc desc = {};
    desc.channels = sound.channels;
    desc.samplerate = sound.sampleRate;
    desc.samples = static_cast<unsigned int>(sound.samples.size() / sound.channels);
    unsigned int length = 0;
    void* bytes = qoa_encode(pcm.data(), &desc, &length);
    if (!bytes)
        return fail("QOA encoding failed");
    sound.encoded.assign(static_cast<uint8_t*>(bytes), static_cast<uint8_t*>(bytes) + length);
    std::free(bytes); // qoa_encode allocates with malloc
    sound.frameCount = desc.samples;
    sound.encoding = SoundEncoding::Qoa;
    return true;
}

uint32_t DecodeQoaFrame(const std::vector<uint8_t>& encoded, uint32_t channels, uint32_t frameIndex, float* out)
{
    const uint64_t offset = kQoaFileHeaderSize + static_cast<uint64_t>(frameIndex) * QoaFrameBytes(channels);
    if (channels == 0 || channels > 2 || offset + 8 > encoded.size())
        return 0;
    qoa_desc desc = {};
    desc.channels = channels;
    // qoa_decode_frame checks the samplerate against the frame header.
    desc.samplerate = (static_cast<uint32_t>(encoded[offset + 1]) << 16) | (static_cast<uint32_t>(encoded[offset + 2]) << 8) | encoded[offset + 3];
    short pcm[kQoaFrameLength * 2];
    unsigned int frameLength = 0;
    if (!qoa_decode_frame(encoded.data() + offset, static_cast<unsigned int>(encoded.size() - offset), &desc, pcm, &frameLength))
        return 0;
    const uint32_t count = frameLength * channels;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = static_cast<float>(pcm[i]) / 32768.0f;
    return frameLength;
}

bool DecodeSoundSamples(SoundFileData& sound, std::string* outError)
{
    if (sound.encoding == SoundEncoding::PcmFloat)
        return true;
    sound.samples.resize(static_cast<size_t>(sound.frameCount) * sound.channels);
    std::vector<float> block(static_cast<size_t>(kQoaFrameLength) * 2);
    uint32_t written = 0;
    for (uint32_t frame = 0; written < sound.frameCount; ++frame)
    {
        const uint32_t decoded = DecodeQoaFrame(sound.encoded, sound.channels, frame, block.data());
        if (decoded == 0)
        {
            if (outError)
                *outError = "Failed to decode QOA sound data";
            sound.samples.clear();
            return false;
        }
        const uint32_t take = (std::min)(decoded, sound.frameCount - written);
        std::copy(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(take) * sound.channels,
            sound.samples.begin() + static_cast<std::ptrdiff_t>(written) * sound.channels);
        written += take;
    }
    sound.encoding = SoundEncoding::PcmFloat;
    sound.encoded.clear();
    sound.encoded.shrink_to_fit();
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

    SoundFileData encodedCopy;
    const SoundFileData* source = &sound;
    if (sound.encoding == SoundEncoding::Qoa && sound.encoded.empty())
    {
        encodedCopy = sound;
        if (!EncodeSoundQoa(encodedCopy, outError))
            return false;
        source = &encodedCopy;
    }
    const bool qoa = source->encoding == SoundEncoding::Qoa;

    const uint32_t derivedFrameCount = qoa ? source->frameCount : static_cast<uint32_t>(sound.samples.size() / sound.channels);
    if (!qoa && sound.frameCount != 0 && sound.frameCount != derivedFrameCount)
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

    const uint32_t payloadCount = static_cast<uint32_t>(qoa ? source->encoded.size() : sound.samples.size());
    if (!WriteValue(file, kKSoundMagic) ||
        !WriteValue(file, kKSoundVersion) ||
        !WriteString(file, sound.sourcePath) ||
        !WriteValue(file, sound.sampleRate) ||
        !WriteValue(file, sound.channels) ||
        !WriteValue(file, derivedFrameCount) ||
        !WriteValue(file, static_cast<uint32_t>(source->encoding)) ||
        !WriteValue(file, payloadCount))
    {
        if (outError)
            *outError = "Failed to write sound header";
        return false;
    }

    if (qoa)
        file.write(reinterpret_cast<const char*>(source->encoded.data()), static_cast<std::streamsize>(source->encoded.size()));
    else if (payloadCount > 0)
        file.write(reinterpret_cast<const char*>(sound.samples.data()), static_cast<std::streamsize>(sound.samples.size() * sizeof(float)));
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write sound samples";
        return false;
    }

    return true;
}

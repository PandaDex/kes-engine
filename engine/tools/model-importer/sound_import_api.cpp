#include "sound_import_api.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "sound_file.h"

namespace
{
    struct DecodedAudioFormat
    {
        GUID subtype = GUID_NULL;
        uint32_t sampleRate = 0;
        uint32_t channels = 0;
        uint32_t bitsPerSample = 0;
    };

    std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            });
        return value;
    }

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};

        const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
        if (count <= 0)
            return {};

        std::wstring wide(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), count);
        return wide;
    }

    template<typename T>
    void SafeRelease(T*& object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    }

    void TrimTrailingWhitespace(std::string& value)
    {
        while (!value.empty())
        {
            const char ch = value.back();
            if (ch != '\r' && ch != '\n' && ch != '\t' && ch != ' ')
                break;
            value.pop_back();
        }
    }

    std::string FormatHResult(HRESULT hr)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);

        char* messageBuffer = nullptr;
        const DWORD chars = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            static_cast<DWORD>(hr),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&messageBuffer),
            0,
            nullptr);
        if (chars > 0 && messageBuffer)
        {
            std::string message(messageBuffer, chars);
            LocalFree(messageBuffer);
            TrimTrailingWhitespace(message);
            if (!message.empty())
                stream << " (" << message << ")";
        }

        return stream.str();
    }

    bool FailHResult(const char* context, HRESULT hr, std::string& error)
    {
        error = std::string(context) + ": " + FormatHResult(hr);
        return false;
    }

    bool PopulateDecodedAudioFormat(IMFSourceReader* reader, DecodedAudioFormat& outFormat, std::string& error)
    {
        IMFMediaType* mediaType = nullptr;
        const HRESULT hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &mediaType);
        if (FAILED(hr))
            return FailHResult("Failed to query decoded audio format", hr, error);

        outFormat = {};
        HRESULT localHr = mediaType->GetGUID(MF_MT_SUBTYPE, &outFormat.subtype);
        if (FAILED(localHr))
        {
            SafeRelease(mediaType);
            return FailHResult("Failed to read decoded audio subtype", localHr, error);
        }

        localHr = mediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &outFormat.sampleRate);
        if (FAILED(localHr))
        {
            SafeRelease(mediaType);
            return FailHResult("Failed to read decoded audio sample rate", localHr, error);
        }

        localHr = mediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outFormat.channels);
        if (FAILED(localHr))
        {
            SafeRelease(mediaType);
            return FailHResult("Failed to read decoded audio channel count", localHr, error);
        }

        localHr = mediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &outFormat.bitsPerSample);
        if (FAILED(localHr))
        {
            if (IsEqualGUID(outFormat.subtype, MFAudioFormat_Float))
                outFormat.bitsPerSample = 32;
            else
            {
                SafeRelease(mediaType);
                return FailHResult("Failed to read decoded audio sample size", localHr, error);
            }
        }

        SafeRelease(mediaType);

        if (outFormat.sampleRate == 0 || outFormat.channels == 0 || outFormat.channels > 2)
        {
            error = "Only mono and stereo sounds with a valid sample rate are supported";
            return false;
        }

        if (IsEqualGUID(outFormat.subtype, MFAudioFormat_Float))
        {
            if (outFormat.bitsPerSample != 32)
            {
                error = "Only 32-bit float decoded audio is supported";
                return false;
            }
            return true;
        }

        if (IsEqualGUID(outFormat.subtype, MFAudioFormat_PCM))
        {
            if (outFormat.bitsPerSample == 8 || outFormat.bitsPerSample == 16 || outFormat.bitsPerSample == 24 || outFormat.bitsPerSample == 32)
                return true;

            error = "Unsupported decoded PCM sample size: " + std::to_string(outFormat.bitsPerSample);
            return false;
        }

        error = "Unsupported decoded audio subtype";
        return false;
    }

    bool ConfigureDecodedAudioOutput(IMFSourceReader* reader, DecodedAudioFormat& outFormat, std::string& error)
    {
        static const GUID kRequestedSubtypes[] = { MFAudioFormat_Float, MFAudioFormat_PCM };

        HRESULT lastHr = S_OK;
        for (const GUID& requestedSubtype : kRequestedSubtypes)
        {
            IMFMediaType* mediaType = nullptr;
            HRESULT hr = MFCreateMediaType(&mediaType);
            if (FAILED(hr))
            {
                lastHr = hr;
                continue;
            }

            hr = mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(hr))
                hr = mediaType->SetGUID(MF_MT_SUBTYPE, requestedSubtype);
            if (SUCCEEDED(hr))
                hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, mediaType);
            SafeRelease(mediaType);
            if (FAILED(hr))
            {
                lastHr = hr;
                continue;
            }

            if (PopulateDecodedAudioFormat(reader, outFormat, error))
                return true;
        }

        return FailHResult("Failed to configure decoded audio output", lastHr, error);
    }

    bool AppendPcmSamples(const BYTE* data, DWORD dataSize, uint32_t bitsPerSample, std::vector<float>& outSamples, std::string& error)
    {
        if (!data)
        {
            error = "Decoded audio buffer is null";
            return false;
        }

        const size_t sampleBytes = bitsPerSample / 8u;
        if (sampleBytes == 0 || (dataSize % sampleBytes) != 0)
        {
            error = "Decoded PCM payload has an invalid size";
            return false;
        }

        const size_t sampleCount = dataSize / sampleBytes;
        outSamples.reserve(outSamples.size() + sampleCount);

        if (bitsPerSample == 8)
        {
            for (size_t index = 0; index < sampleCount; ++index)
                outSamples.push_back((static_cast<int>(data[index]) - 128) / 128.0f);
            return true;
        }

        if (bitsPerSample == 16)
        {
            for (size_t index = 0; index < sampleCount; ++index)
            {
                int16_t sample = 0;
                std::memcpy(&sample, data + index * 2u, sizeof(sample));
                outSamples.push_back(static_cast<float>(sample) / 32768.0f);
            }
            return true;
        }

        if (bitsPerSample == 24)
        {
            for (size_t index = 0; index < sampleCount; ++index)
            {
                const BYTE* src = data + index * 3u;
                int32_t sample = static_cast<int32_t>(src[0]) |
                    (static_cast<int32_t>(src[1]) << 8) |
                    (static_cast<int32_t>(src[2]) << 16);
                if ((sample & 0x00800000) != 0)
                    sample |= ~0x00FFFFFF;
                outSamples.push_back(static_cast<float>(sample) / 8388608.0f);
            }
            return true;
        }

        if (bitsPerSample == 32)
        {
            for (size_t index = 0; index < sampleCount; ++index)
            {
                int32_t sample = 0;
                std::memcpy(&sample, data + index * 4u, sizeof(sample));
                outSamples.push_back(static_cast<float>(sample) / 2147483648.0f);
            }
            return true;
        }

        error = "Unsupported PCM sample size";
        return false;
    }

    bool AppendDecodedSamples(const BYTE* data, DWORD dataSize, const DecodedAudioFormat& format, std::vector<float>& outSamples, std::string& error)
    {
        if (dataSize == 0)
            return true;

        if (IsEqualGUID(format.subtype, MFAudioFormat_Float))
        {
            if ((dataSize % sizeof(float)) != 0)
            {
                error = "Decoded float audio payload has an invalid size";
                return false;
            }

            const size_t sampleCount = dataSize / sizeof(float);
            const size_t offset = outSamples.size();
            outSamples.resize(offset + sampleCount);
            std::memcpy(outSamples.data() + offset, data, dataSize);
            return true;
        }

        if (IsEqualGUID(format.subtype, MFAudioFormat_PCM))
            return AppendPcmSamples(data, dataSize, format.bitsPerSample, outSamples, error);

        error = "Unsupported decoded audio subtype";
        return false;
    }
}

bool CanImportSoundSource(const std::string& path)
{
    const std::string extension = Lowercase(std::filesystem::path(path).extension().string());
    return extension == ".wav" || extension == ".mp3" || extension == ".ogg";
}

bool ImportSoundToKSound(const std::string& inputPath, const std::string& outputPath, std::string& error)
{
    if (!CanImportSoundSource(inputPath))
    {
        error = "Unsupported source format: " + Lowercase(std::filesystem::path(inputPath).extension().string());
        return false;
    }

    const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = initResult == S_OK || initResult == S_FALSE;
    if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE)
        return FailHResult("Failed to initialize COM", initResult, error);

    const HRESULT mfResult = MFStartup(MF_VERSION);
    if (FAILED(mfResult))
    {
        if (shouldUninitialize)
            CoUninitialize();
        return FailHResult("Failed to initialize Media Foundation", mfResult, error);
    }

    IMFSourceReader* reader = nullptr;
    const std::wstring inputPathWide = Utf8ToWide(inputPath);
    if (inputPathWide.empty())
    {
        MFShutdown();
        if (shouldUninitialize)
            CoUninitialize();
        error = "Failed to convert input path";
        return false;
    }

    HRESULT hr = MFCreateSourceReaderFromURL(inputPathWide.c_str(), nullptr, &reader);
    if (FAILED(hr))
    {
        MFShutdown();
        if (shouldUninitialize)
            CoUninitialize();
        return FailHResult("Failed to open source audio", hr, error);
    }

    hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr))
        hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr))
    {
        SafeRelease(reader);
        MFShutdown();
        if (shouldUninitialize)
            CoUninitialize();
        return FailHResult("Failed to select source audio stream", hr, error);
    }

    DecodedAudioFormat format = {};
    if (!ConfigureDecodedAudioOutput(reader, format, error))
    {
        SafeRelease(reader);
        MFShutdown();
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    }

    SoundFileData sound = {};
    sound.sourcePath = inputPath;
    sound.sampleRate = format.sampleRate;
    sound.channels = format.channels;

    bool success = true;
    while (success)
    {
        DWORD streamIndex = 0;
        DWORD streamFlags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &streamFlags, nullptr, &sample);
        if (FAILED(hr))
        {
            success = FailHResult("Failed to decode source audio", hr, error);
            SafeRelease(sample);
            break;
        }

        if ((streamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
        {
            DecodedAudioFormat updatedFormat = {};
            if (!PopulateDecodedAudioFormat(reader, updatedFormat, error))
            {
                success = false;
                SafeRelease(sample);
                break;
            }

            if (updatedFormat.sampleRate != sound.sampleRate || updatedFormat.channels != sound.channels)
            {
                error = "Decoded audio format changed mid-stream";
                success = false;
                SafeRelease(sample);
                break;
            }

            format = updatedFormat;
        }

        if (sample)
        {
            IMFMediaBuffer* buffer = nullptr;
            BYTE* data = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;

            hr = sample->ConvertToContiguousBuffer(&buffer);
            if (FAILED(hr))
            {
                success = FailHResult("Failed to access decoded audio buffer", hr, error);
                SafeRelease(sample);
                break;
            }

            hr = buffer->Lock(&data, &maxLength, &currentLength);
            if (FAILED(hr))
            {
                SafeRelease(buffer);
                SafeRelease(sample);
                success = FailHResult("Failed to lock decoded audio buffer", hr, error);
                break;
            }

            success = AppendDecodedSamples(data, currentLength, format, sound.samples, error);
            buffer->Unlock();
            SafeRelease(buffer);
        }

        SafeRelease(sample);

        if (!success)
            break;

        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            break;
    }

    SafeRelease(reader);
    MFShutdown();
    if (shouldUninitialize)
        CoUninitialize();

    if (!success)
        return false;

    if ((sound.samples.size() % sound.channels) != 0)
    {
        error = "Decoded audio sample count does not align to channel count";
        return false;
    }

    sound.frameCount = static_cast<uint32_t>(sound.samples.size() / sound.channels);
    return SaveKSoundFile(outputPath.c_str(), sound, &error);
}

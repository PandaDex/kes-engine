#include "animation_clip.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "kmodel.h"
#include "pak_file_system.h"
#include "profiler.h"

namespace
{
    constexpr uint32_t kKAnimMagic = 0x4D494E41u; // ANIM
    constexpr uint32_t kKAnimVersion = 1u;

    template<typename T>
    static bool WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return file.good();
    }

    template<typename T>
    static bool ReadValue(std::istream& file, T& value)
    {
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
        return file.good();
    }

    static bool WriteString(std::ofstream& file, const std::string& value)
    {
        const uint32_t length = (uint32_t)value.size();
        if (!WriteValue(file, length))
            return false;
        if (length > 0)
            file.write(value.data(), length);
        return file.good();
    }

    static bool ReadString(std::istream& file, std::string& value)
    {
        uint32_t length = 0;
        if (!ReadValue(file, length))
            return false;
        value.resize(length);
        if (length > 0)
            file.read(value.data(), length);
        return file.good();
    }

    static bool WriteFloatArray(std::ofstream& file, const float* values, size_t count)
    {
        file.write(reinterpret_cast<const char*>(values), (std::streamsize)(sizeof(float) * count));
        return file.good();
    }

    static bool ReadFloatArray(std::istream& file, float* values, size_t count)
    {
        file.read(reinterpret_cast<char*>(values), (std::streamsize)(sizeof(float) * count));
        return file.good();
    }

    static float WrapAnimationTime(float timeSeconds, float duration)
    {
        if (duration <= 0.000001f)
            return 0.0f;
        const float loops = std::floor(timeSeconds / duration);
        float wrapped = timeSeconds - loops * duration;
        if (wrapped < 0.0f)
            wrapped += duration;
        return wrapped;
    }

    static void NormalizeQuaternion(float value[4])
    {
        const float lengthSq = value[0] * value[0] + value[1] * value[1] + value[2] * value[2] + value[3] * value[3];
        if (lengthSq <= 0.000001f)
        {
            value[0] = 0.0f;
            value[1] = 0.0f;
            value[2] = 0.0f;
            value[3] = 1.0f;
            return;
        }

        const float invLength = 1.0f / std::sqrt(lengthSq);
        value[0] *= invLength;
        value[1] *= invLength;
        value[2] *= invLength;
        value[3] *= invLength;
    }

    static void SampleVec3Keys(const std::vector<AnimationVec3Key>& keys, float timeSeconds, float outValue[3])
    {
        if (keys.empty())
            return;

        if (keys.size() == 1 || timeSeconds <= keys.front().time)
        {
            outValue[0] = keys.front().value[0];
            outValue[1] = keys.front().value[1];
            outValue[2] = keys.front().value[2];
            return;
        }

        if (timeSeconds >= keys.back().time)
        {
            outValue[0] = keys.back().value[0];
            outValue[1] = keys.back().value[1];
            outValue[2] = keys.back().value[2];
            return;
        }

        for (size_t index = 1; index < keys.size(); ++index)
        {
            const AnimationVec3Key& next = keys[index];
            if (timeSeconds > next.time)
                continue;

            const AnimationVec3Key& prev = keys[index - 1];
            const float span = next.time - prev.time;
            const float t = span > 0.000001f ? (timeSeconds - prev.time) / span : 0.0f;
            for (int component = 0; component < 3; ++component)
                outValue[component] = prev.value[component] + (next.value[component] - prev.value[component]) * t;
            return;
        }
    }

    static void SampleQuatKeys(const std::vector<AnimationQuatKey>& keys, float timeSeconds, float outValue[4])
    {
        if (keys.empty())
            return;

        if (keys.size() == 1 || timeSeconds <= keys.front().time)
        {
            for (int component = 0; component < 4; ++component)
                outValue[component] = keys.front().value[component];
            return;
        }

        if (timeSeconds >= keys.back().time)
        {
            for (int component = 0; component < 4; ++component)
                outValue[component] = keys.back().value[component];
            return;
        }

        for (size_t index = 1; index < keys.size(); ++index)
        {
            const AnimationQuatKey& next = keys[index];
            if (timeSeconds > next.time)
                continue;

            const AnimationQuatKey& prev = keys[index - 1];
            const float span = next.time - prev.time;
            const float t = span > 0.000001f ? (timeSeconds - prev.time) / span : 0.0f;

            float from[4] = { prev.value[0], prev.value[1], prev.value[2], prev.value[3] };
            float to[4] = { next.value[0], next.value[1], next.value[2], next.value[3] };
            const float dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2] + from[3] * to[3];
            if (dot < 0.0f)
            {
                for (float& component : to)
                    component = -component;
            }

            for (int component = 0; component < 4; ++component)
                outValue[component] = from[component] + (to[component] - from[component]) * t;
            NormalizeQuaternion(outValue);
            return;
        }
    }
}

bool LoadAnimationClipFile(const char* path, AnimationClipData& outClip, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outClip = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;

    const std::string fileData(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    std::istringstream file(fileData, std::ios::binary);

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!ReadValue(file, magic) ||
        !ReadValue(file, version) ||
        !ReadString(file, outClip.name) ||
        !ReadString(file, outClip.sourcePath) ||
        !ReadString(file, outClip.modelPath) ||
        !ReadValue(file, outClip.duration) ||
        !ReadValue(file, outClip.looping))
    {
        if (outError)
            *outError = "Failed to read animation clip header";
        return false;
    }

    if (magic != kKAnimMagic)
    {
        if (outError)
            *outError = "Invalid animation clip magic";
        return false;
    }

    if (version != kKAnimVersion)
    {
        if (outError)
            *outError = "Unsupported animation clip version";
        return false;
    }

    uint32_t trackCount = 0;
    if (!ReadValue(file, trackCount))
    {
        if (outError)
            *outError = "Failed to read animation clip track count";
        return false;
    }

    outClip.tracks.resize(trackCount);
    for (AnimationNodeTrack& track : outClip.tracks)
    {
        uint32_t translationCount = 0;
        uint32_t rotationCount = 0;
        uint32_t scaleCount = 0;
        if (!ReadString(file, track.nodeName) ||
            !ReadValue(file, track.nodeIndex) ||
            !ReadValue(file, translationCount) ||
            !ReadValue(file, rotationCount) ||
            !ReadValue(file, scaleCount))
        {
            if (outError)
                *outError = "Failed to read animation track header";
            return false;
        }

        track.translationKeys.resize(translationCount);
        track.rotationKeys.resize(rotationCount);
        track.scaleKeys.resize(scaleCount);

        for (AnimationVec3Key& key : track.translationKeys)
        {
            if (!ReadValue(file, key.time) || !ReadFloatArray(file, key.value, 3))
            {
                if (outError)
                    *outError = "Failed to read translation key";
                return false;
            }
        }

        for (AnimationQuatKey& key : track.rotationKeys)
        {
            if (!ReadValue(file, key.time) || !ReadFloatArray(file, key.value, 4))
            {
                if (outError)
                    *outError = "Failed to read rotation key";
                return false;
            }
        }

        for (AnimationVec3Key& key : track.scaleKeys)
        {
            if (!ReadValue(file, key.time) || !ReadFloatArray(file, key.value, 3))
            {
                if (outError)
                    *outError = "Failed to read scale key";
                return false;
            }
        }
    }

    return true;
}

bool SaveAnimationClipFile(const char* path, const AnimationClipData& clip, std::string* outError)
{
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing animation clip output path";
        return false;
    }

    const std::filesystem::path outputPath(path);
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path());

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open animation clip output file";
        return false;
    }

    if (!WriteValue(file, kKAnimMagic) ||
        !WriteValue(file, kKAnimVersion) ||
        !WriteString(file, clip.name) ||
        !WriteString(file, clip.sourcePath) ||
        !WriteString(file, clip.modelPath) ||
        !WriteValue(file, clip.duration) ||
        !WriteValue(file, clip.looping))
    {
        if (outError)
            *outError = "Failed to write animation clip header";
        return false;
    }

    const uint32_t trackCount = (uint32_t)clip.tracks.size();
    if (!WriteValue(file, trackCount))
    {
        if (outError)
            *outError = "Failed to write animation clip track count";
        return false;
    }

    for (const AnimationNodeTrack& track : clip.tracks)
    {
        const uint32_t translationCount = (uint32_t)track.translationKeys.size();
        const uint32_t rotationCount = (uint32_t)track.rotationKeys.size();
        const uint32_t scaleCount = (uint32_t)track.scaleKeys.size();
        if (!WriteString(file, track.nodeName) ||
            !WriteValue(file, track.nodeIndex) ||
            !WriteValue(file, translationCount) ||
            !WriteValue(file, rotationCount) ||
            !WriteValue(file, scaleCount))
        {
            if (outError)
                *outError = "Failed to write animation track header";
            return false;
        }

        for (const AnimationVec3Key& key : track.translationKeys)
        {
            if (!WriteValue(file, key.time) || !WriteFloatArray(file, key.value, 3))
            {
                if (outError)
                    *outError = "Failed to write translation key";
                return false;
            }
        }

        for (const AnimationQuatKey& key : track.rotationKeys)
        {
            if (!WriteValue(file, key.time) || !WriteFloatArray(file, key.value, 4))
            {
                if (outError)
                    *outError = "Failed to write rotation key";
                return false;
            }
        }

        for (const AnimationVec3Key& key : track.scaleKeys)
        {
            if (!WriteValue(file, key.time) || !WriteFloatArray(file, key.value, 3))
            {
                if (outError)
                    *outError = "Failed to write scale key";
                return false;
            }
        }
    }

    return true;
}

void BuildAnimationBindPose(const KModelData& model, std::vector<AnimationTransform>& outLocalPose)
{
    outLocalPose.clear();
    outLocalPose.resize(model.nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
    {
        const KModelNode& node = model.nodes[nodeIndex];
        AnimationTransform& transform = outLocalPose[nodeIndex];
        for (int i = 0; i < 3; ++i)
        {
            transform.translation[i] = node.translation[i];
            transform.scale[i] = node.scale[i];
        }
        for (int i = 0; i < 4; ++i)
            transform.rotation[i] = node.rotation[i];
    }
}

bool SampleAnimationClip(const KModelData& model, const AnimationClipData& clip, float timeSeconds, std::vector<AnimationTransform>& outLocalPose, std::string* outError)
{
    KES_ZONE_FN(WORLD);
    BuildAnimationBindPose(model, outLocalPose);

    const float sampleTime = clip.looping ? WrapAnimationTime(timeSeconds, clip.duration) : (timeSeconds < 0.0f ? 0.0f : timeSeconds);
    for (const AnimationNodeTrack& track : clip.tracks)
    {
        if (track.nodeIndex < 0 || track.nodeIndex >= (int32_t)outLocalPose.size())
            continue;

        AnimationTransform& transform = outLocalPose[(size_t)track.nodeIndex];
        SampleVec3Keys(track.translationKeys, sampleTime, transform.translation);
        SampleQuatKeys(track.rotationKeys, sampleTime, transform.rotation);
        SampleVec3Keys(track.scaleKeys, sampleTime, transform.scale);
    }

    (void)outError;
    return true;
}

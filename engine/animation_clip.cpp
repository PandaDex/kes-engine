#include "animation_clip.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "binary_reader.h"
#include "kmodel.h"
#include "asset_file_io.h"
#include "profiler.h"

namespace
{
    constexpr uint32_t kKAnimMagic = 0x4D494E41u; // ANIM
    constexpr uint32_t kKAnimVersion = 1u;

    // Keys are time followed by the value, the same in memory and in the file.
    static_assert(sizeof(AnimationVec3Key) == 16 && sizeof(AnimationQuatKey) == 20, "Animation key file layout");

    // First key at or after `timeSeconds`. The caller has handled the ends of
    // the track, so the result is never the first key.
    template<typename Key>
    static size_t FindNextKey(const std::vector<Key>& keys, float timeSeconds)
    {
        const auto next = std::lower_bound(keys.begin(), keys.end(), timeSeconds,
                                           [](const Key& key, float time) { return key.time < time; });
        return static_cast<size_t>(next - keys.begin());
    }

    template<typename T>
    static bool WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
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

    static bool WriteFloatArray(std::ofstream& file, const float* values, size_t count)
    {
        file.write(reinterpret_cast<const char*>(values), (std::streamsize)(sizeof(float) * count));
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

        const size_t index = FindNextKey(keys, timeSeconds);
        const AnimationVec3Key& next = keys[index];
        const AnimationVec3Key& prev = keys[index - 1];
        const float span = next.time - prev.time;
        const float t = span > 0.000001f ? (timeSeconds - prev.time) / span : 0.0f;
        for (int component = 0; component < 3; ++component)
            outValue[component] = prev.value[component] + (next.value[component] - prev.value[component]) * t;
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

        const size_t index = FindNextKey(keys, timeSeconds);
        const AnimationQuatKey& next = keys[index];
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

    const auto fail = [&](const char* message)
    {
        if (outError)
            *outError = message;
        outClip = {};
        return false;
    };

    BinaryReader file(fileBytes);
    const uint32_t magic = file.u32();
    const uint32_t version = file.u32();
    if (!file.ok())
        return fail("Failed to read animation clip header");
    if (magic != kKAnimMagic)
        return fail("Invalid animation clip magic");
    if (version != kKAnimVersion)
        return fail("Unsupported animation clip version");

    outClip.name = file.string();
    outClip.sourcePath = file.string();
    outClip.modelPath = file.string();
    outClip.duration = file.f32();
    outClip.looping = file.u8() != 0;

    const uint32_t trackCount = file.u32();
    if (!file.count(trackCount, 20))
        return fail("Failed to read animation clip header");

    outClip.tracks.resize(trackCount);
    for (AnimationNodeTrack& track : outClip.tracks)
    {
        track.nodeName = file.string();
        track.nodeIndex = file.i32();
        const uint32_t translationCount = file.u32();
        const uint32_t rotationCount = file.u32();
        const uint32_t scaleCount = file.u32();
        if (!file.count(translationCount, sizeof(AnimationVec3Key)))
            return fail("Failed to read animation track");
        track.translationKeys.resize(translationCount);
        file.bytes(track.translationKeys.data(), translationCount * sizeof(AnimationVec3Key));

        if (!file.count(rotationCount, sizeof(AnimationQuatKey)))
            return fail("Failed to read animation track");
        track.rotationKeys.resize(rotationCount);
        file.bytes(track.rotationKeys.data(), rotationCount * sizeof(AnimationQuatKey));

        if (!file.count(scaleCount, sizeof(AnimationVec3Key)))
            return fail("Failed to read animation track");
        track.scaleKeys.resize(scaleCount);
        file.bytes(track.scaleKeys.data(), scaleCount * sizeof(AnimationVec3Key));
    }

    if (!file.ok())
        return fail("Failed to read animation track");
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

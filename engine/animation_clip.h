#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct KModelData;
using AnimationClipHandle = uint32_t;

struct AnimationTransform
{
    float translation[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

struct AnimationVec3Key
{
    float time = 0.0f;
    float value[3] = { 0.0f, 0.0f, 0.0f };
};

struct AnimationQuatKey
{
    float time = 0.0f;
    float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct AnimationNodeTrack
{
    std::string nodeName;
    int32_t nodeIndex = -1;
    std::vector<AnimationVec3Key> translationKeys;
    std::vector<AnimationQuatKey> rotationKeys;
    std::vector<AnimationVec3Key> scaleKeys;
};

struct AnimationClipData
{
    std::string name;
    std::string sourcePath;
    std::string modelPath;
    float duration = 0.0f;
    bool looping = true;
    std::vector<AnimationNodeTrack> tracks;
};

bool LoadAnimationClipFile(const char* path, AnimationClipData& outClip, std::string* outError = nullptr);
bool SaveAnimationClipFile(const char* path, const AnimationClipData& clip, std::string* outError = nullptr);

void BuildAnimationBindPose(const KModelData& model, std::vector<AnimationTransform>& outLocalPose);
bool SampleAnimationClip(const KModelData& model, const AnimationClipData& clip, float timeSeconds, std::vector<AnimationTransform>& outLocalPose, std::string* outError = nullptr);

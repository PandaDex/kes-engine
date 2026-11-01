#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "engine_types.h"

using AnimationGraphHandle = uint32_t;

enum class AnimationGraphParameterType
{
    Bool,
    Float,
    Trigger
};

enum class AnimationGraphConditionType
{
    BoolTrue,
    BoolFalse,
    FloatGreater,
    FloatLess,
    Triggered
};

struct AnimationGraphParameter
{
    std::string name;
    AnimationGraphParameterType type = AnimationGraphParameterType::Bool;
    bool defaultBoolValue = false;
    float defaultFloatValue = 0.0f;
};

struct AnimationGraphCondition
{
    std::string parameter;
    AnimationGraphConditionType type = AnimationGraphConditionType::BoolTrue;
    float threshold = 0.0f;
};

struct AnimationGraphState
{
    std::string name;
    std::string clipPath;
    Vector2 position = {};
    float speed = 1.0f;
    bool looping = true;
};

struct AnimationGraphTransition
{
    std::string fromState;
    std::string toState;
    float blendTime = 0.1f;
    float exitTime = -1.0f;
    std::vector<AnimationGraphCondition> conditions;
};

struct AnimationGraphData
{
    std::string name;
    std::string modelPath;
    std::string entryState;
    std::vector<AnimationGraphParameter> parameters;
    std::vector<AnimationGraphState> states;
    std::vector<AnimationGraphTransition> transitions;
};

bool LoadAnimationGraphFile(const char* path, AnimationGraphData& outGraph, std::string* outError = nullptr);
bool SaveAnimationGraphFile(const char* path, const AnimationGraphData& graph, std::string* outError = nullptr);

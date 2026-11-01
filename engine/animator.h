#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "animation_graph.h"

struct AnimatorPlaybackState
{
    std::string clipPath;
    float clipTime = 0.0f;
    float speed = 1.0f;
    bool looping = true;
};

struct AnimatorGraphInstance
{
    int32_t currentStateIndex = -1;
    float stateTime = 0.0f;
    std::vector<bool> boolParameters;
    std::vector<float> floatParameters;
    std::vector<bool> triggerParameters;
};

void InitializeAnimatorGraphInstance(const AnimationGraphData& graph, AnimatorGraphInstance& outInstance);
void SetAnimatorBool(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const char* name, bool value);
void SetAnimatorFloat(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const char* name, float value);
void FireAnimatorTrigger(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const char* name);
bool UpdateAnimatorGraph(const AnimationGraphData& graph, AnimatorGraphInstance& instance, float dt, AnimatorPlaybackState& outPlayback);

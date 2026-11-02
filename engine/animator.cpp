#include "animator.h"
#include "profiler.h"

namespace
{
    int32_t FindParameterIndex(const AnimationGraphData& graph, const char* name, AnimationGraphParameterType type)
    {
        if (!name)
            return -1;

        for (size_t index = 0; index < graph.parameters.size(); ++index)
        {
            const AnimationGraphParameter& parameter = graph.parameters[index];
            if (parameter.name == name && parameter.type == type)
                return (int32_t)index;
        }

        return -1;
    }

    int32_t FindStateIndex(const AnimationGraphData& graph, const std::string& name)
    {
        for (size_t index = 0; index < graph.states.size(); ++index)
        {
            if (graph.states[index].name == name)
                return (int32_t)index;
        }
        return -1;
    }

    bool EvaluateCondition(const AnimationGraphData& graph, const AnimatorGraphInstance& instance, const AnimationGraphCondition& condition)
    {
        switch (condition.type)
        {
        case AnimationGraphConditionType::BoolTrue:
        case AnimationGraphConditionType::BoolFalse:
        {
            const int32_t parameterIndex = FindParameterIndex(graph, condition.parameter.c_str(), AnimationGraphParameterType::Bool);
            if (parameterIndex < 0 || parameterIndex >= (int32_t)instance.boolParameters.size())
                return false;
            return condition.type == AnimationGraphConditionType::BoolTrue ? instance.boolParameters[(size_t)parameterIndex] : !instance.boolParameters[(size_t)parameterIndex];
        }
        case AnimationGraphConditionType::FloatGreater:
        case AnimationGraphConditionType::FloatLess:
        {
            const int32_t parameterIndex = FindParameterIndex(graph, condition.parameter.c_str(), AnimationGraphParameterType::Float);
            if (parameterIndex < 0 || parameterIndex >= (int32_t)instance.floatParameters.size())
                return false;
            return condition.type == AnimationGraphConditionType::FloatGreater
                ? instance.floatParameters[(size_t)parameterIndex] > condition.threshold
                : instance.floatParameters[(size_t)parameterIndex] < condition.threshold;
        }
        case AnimationGraphConditionType::Triggered:
        {
            const int32_t parameterIndex = FindParameterIndex(graph, condition.parameter.c_str(), AnimationGraphParameterType::Trigger);
            if (parameterIndex < 0 || parameterIndex >= (int32_t)instance.triggerParameters.size())
                return false;
            return instance.triggerParameters[(size_t)parameterIndex];
        }
        default:
            return false;
        }
    }

    bool TransitionCanFire(const AnimationGraphData& graph, const AnimatorGraphInstance& instance, const AnimationGraphTransition& transition)
    {
        for (const AnimationGraphCondition& condition : transition.conditions)
        {
            if (!EvaluateCondition(graph, instance, condition))
                return false;
        }
        return true;
    }

    void ConsumeTransitionTriggers(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const AnimationGraphTransition& transition)
    {
        for (const AnimationGraphCondition& condition : transition.conditions)
        {
            if (condition.type != AnimationGraphConditionType::Triggered)
                continue;

            const int32_t parameterIndex = FindParameterIndex(graph, condition.parameter.c_str(), AnimationGraphParameterType::Trigger);
            if (parameterIndex >= 0 && parameterIndex < (int32_t)instance.triggerParameters.size())
                instance.triggerParameters[(size_t)parameterIndex] = false;
        }
    }
}

void InitializeAnimatorGraphInstance(const AnimationGraphData& graph, AnimatorGraphInstance& outInstance)
{
    outInstance = {};
    outInstance.boolParameters.resize(graph.parameters.size(), false);
    outInstance.floatParameters.resize(graph.parameters.size(), 0.0f);
    outInstance.triggerParameters.resize(graph.parameters.size(), false);

    for (size_t index = 0; index < graph.parameters.size(); ++index)
    {
        const AnimationGraphParameter& parameter = graph.parameters[index];
        outInstance.boolParameters[index] = parameter.defaultBoolValue;
        outInstance.floatParameters[index] = parameter.defaultFloatValue;
    }

    outInstance.currentStateIndex = FindStateIndex(graph, graph.entryState);
    if (outInstance.currentStateIndex < 0 && !graph.states.empty())
        outInstance.currentStateIndex = 0;
}

void SetAnimatorBool(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const char* name, bool value)
{
    const int32_t parameterIndex = FindParameterIndex(graph, name, AnimationGraphParameterType::Bool);
    if (parameterIndex >= 0 && parameterIndex < (int32_t)instance.boolParameters.size())
        instance.boolParameters[(size_t)parameterIndex] = value;
}

void SetAnimatorFloat(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const char* name, float value)
{
    const int32_t parameterIndex = FindParameterIndex(graph, name, AnimationGraphParameterType::Float);
    if (parameterIndex >= 0 && parameterIndex < (int32_t)instance.floatParameters.size())
        instance.floatParameters[(size_t)parameterIndex] = value;
}

void FireAnimatorTrigger(const AnimationGraphData& graph, AnimatorGraphInstance& instance, const char* name)
{
    const int32_t parameterIndex = FindParameterIndex(graph, name, AnimationGraphParameterType::Trigger);
    if (parameterIndex >= 0 && parameterIndex < (int32_t)instance.triggerParameters.size())
        instance.triggerParameters[(size_t)parameterIndex] = true;
}

bool UpdateAnimatorGraph(const AnimationGraphData& graph, AnimatorGraphInstance& instance, float dt, AnimatorPlaybackState& outPlayback)
{
    KES_ZONE_FN(WORLD);
    outPlayback = {};
    if (graph.states.empty())
        return false;

    if (instance.currentStateIndex < 0 || instance.currentStateIndex >= (int32_t)graph.states.size())
    {
        InitializeAnimatorGraphInstance(graph, instance);
        if (instance.currentStateIndex < 0 || instance.currentStateIndex >= (int32_t)graph.states.size())
            return false;
    }

    instance.stateTime += dt;

    const AnimationGraphState* state = &graph.states[(size_t)instance.currentStateIndex];
    for (const AnimationGraphTransition& transition : graph.transitions)
    {
        if (transition.fromState != state->name)
            continue;
        if (transition.exitTime >= 0.0f && instance.stateTime < transition.exitTime)
            continue;
        if (!TransitionCanFire(graph, instance, transition))
            continue;

        const int32_t nextStateIndex = FindStateIndex(graph, transition.toState);
        if (nextStateIndex >= 0 && nextStateIndex < (int32_t)graph.states.size())
        {
            ConsumeTransitionTriggers(graph, instance, transition);
            instance.currentStateIndex = nextStateIndex;
            instance.stateTime = 0.0f;
            state = &graph.states[(size_t)instance.currentStateIndex];
            break;
        }
    }

    outPlayback.clipPath = state->clipPath;
    outPlayback.clipTime = instance.stateTime * state->speed;
    outPlayback.speed = state->speed;
    outPlayback.looping = state->looping;
    return true;
}

#include "animation_graph.h"
#include <filesystem>
#include <fstream>
#include "file_paths.h"
#include "text_block_file.h"
#include "text_parse_utils.h"

namespace
{
    const char* GetParameterTypeName(AnimationGraphParameterType type)
    {
        switch (type)
        {
        case AnimationGraphParameterType::Float: return "Float";
        case AnimationGraphParameterType::Trigger: return "Trigger";
        default: return "Bool";
        }
    }

    AnimationGraphParameterType ParseParameterType(const std::string& value)
    {
        const std::string cleaned = textparse::Unquote(value);
        if (cleaned == "Float")
            return AnimationGraphParameterType::Float;
        if (cleaned == "Trigger")
            return AnimationGraphParameterType::Trigger;
        return AnimationGraphParameterType::Bool;
    }

    const char* GetConditionTypeName(AnimationGraphConditionType type)
    {
        switch (type)
        {
        case AnimationGraphConditionType::BoolFalse: return "BoolFalse";
        case AnimationGraphConditionType::FloatGreater: return "FloatGreater";
        case AnimationGraphConditionType::FloatLess: return "FloatLess";
        case AnimationGraphConditionType::Triggered: return "Triggered";
        default: return "BoolTrue";
        }
    }

    AnimationGraphConditionType ParseConditionType(const std::string& value)
    {
        const std::string cleaned = textparse::Unquote(value);
        if (cleaned == "BoolFalse")
            return AnimationGraphConditionType::BoolFalse;
        if (cleaned == "FloatGreater")
            return AnimationGraphConditionType::FloatGreater;
        if (cleaned == "FloatLess")
            return AnimationGraphConditionType::FloatLess;
        if (cleaned == "Triggered")
            return AnimationGraphConditionType::Triggered;
        return AnimationGraphConditionType::BoolTrue;
    }
}

bool LoadAnimationGraphFile(const char* path, AnimationGraphData& outGraph, std::string* outError)
{
    outGraph = {};

    std::vector<TextBlock> blocks;
    if (!LoadTextBlockFile(path, blocks, outError))
        return false;

    for (const TextBlock& block : blocks)
    {
        if (block.type == "AnimGraph")
        {
            if (const std::string* value = FindTextBlockProperty(block, "Name"))
                outGraph.name = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "Model"))
                outGraph.modelPath = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "EntryState"))
                outGraph.entryState = textparse::Unquote(*value);
        }
        else if (block.type == "Parameter")
        {
            AnimationGraphParameter parameter = {};
            if (const std::string* value = FindTextBlockProperty(block, "Name"))
                parameter.name = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "Type"))
                parameter.type = ParseParameterType(*value);
            if (const std::string* value = FindTextBlockProperty(block, "DefaultBool"))
                parameter.defaultBoolValue = textparse::ParseBool(*value, parameter.defaultBoolValue);
            if (const std::string* value = FindTextBlockProperty(block, "DefaultFloat"))
                parameter.defaultFloatValue = textparse::ParseFloat(*value, parameter.defaultFloatValue);
            outGraph.parameters.push_back(parameter);
        }
        else if (block.type == "State")
        {
            AnimationGraphState state = {};
            if (const std::string* value = FindTextBlockProperty(block, "Name"))
                state.name = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "Clip"))
                state.clipPath = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "Position"))
                state.position = textparse::ParseVector2(*value, {});
            if (const std::string* value = FindTextBlockProperty(block, "Speed"))
                state.speed = textparse::ParseFloat(*value, state.speed);
            if (const std::string* value = FindTextBlockProperty(block, "Loop"))
                state.looping = textparse::ParseBool(*value, state.looping);
            outGraph.states.push_back(state);
        }
        else if (block.type == "Transition")
        {
            AnimationGraphTransition transition = {};
            if (const std::string* value = FindTextBlockProperty(block, "From"))
                transition.fromState = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "To"))
                transition.toState = textparse::Unquote(*value);
            if (const std::string* value = FindTextBlockProperty(block, "BlendTime"))
                transition.blendTime = textparse::ParseFloat(*value, transition.blendTime);
            if (const std::string* value = FindTextBlockProperty(block, "ExitTime"))
                transition.exitTime = textparse::ParseFloat(*value, transition.exitTime);
            outGraph.transitions.push_back(transition);
        }
        else if (block.type == "Condition")
        {
            const std::string* fromValue = FindTextBlockProperty(block, "From");
            const std::string* toValue = FindTextBlockProperty(block, "To");
            if (!fromValue || !toValue)
                continue;

            for (AnimationGraphTransition& transition : outGraph.transitions)
            {
                if (transition.fromState != textparse::Unquote(*fromValue) ||
                    transition.toState != textparse::Unquote(*toValue))
                    continue;

                AnimationGraphCondition condition = {};
                if (const std::string* value = FindTextBlockProperty(block, "Parameter"))
                    condition.parameter = textparse::Unquote(*value);
                if (const std::string* value = FindTextBlockProperty(block, "Type"))
                    condition.type = ParseConditionType(*value);
                if (const std::string* value = FindTextBlockProperty(block, "Threshold"))
                    condition.threshold = textparse::ParseFloat(*value, condition.threshold);
                transition.conditions.push_back(condition);
                break;
            }
        }
    }

    return true;
}

bool SaveAnimationGraphFile(const char* path, const AnimationGraphData& graph, std::string* outError)
{
    const std::string resolvedPath = ResolveAssetWritePath(path);
    const std::filesystem::path outputPath(resolvedPath);
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open animation graph file for save";
        return false;
    }

    file << "AnimGraph {\n";
    if (!graph.name.empty())
        file << "    Name = " << textparse::Quote(graph.name) << "\n";
    if (!graph.modelPath.empty())
        file << "    Model = " << textparse::Quote(graph.modelPath) << "\n";
    if (!graph.entryState.empty())
        file << "    EntryState = " << textparse::Quote(graph.entryState) << "\n";
    file << "}\n";

    for (const AnimationGraphParameter& parameter : graph.parameters)
    {
        file << "\nParameter {\n";
        file << "    Name = " << textparse::Quote(parameter.name) << "\n";
        file << "    Type = " << textparse::Quote(GetParameterTypeName(parameter.type)) << "\n";
        file << "    DefaultBool = " << (parameter.defaultBoolValue ? "true" : "false") << "\n";
        file << "    DefaultFloat = " << parameter.defaultFloatValue << "\n";
        file << "}\n";
    }

    for (const AnimationGraphState& state : graph.states)
    {
        file << "\nState {\n";
        file << "    Name = " << textparse::Quote(state.name) << "\n";
        if (!state.clipPath.empty())
            file << "    Clip = " << textparse::Quote(state.clipPath) << "\n";
        file << "    Position = \"" << state.position.x << "," << state.position.y << "\"\n";
        file << "    Speed = " << state.speed << "\n";
        file << "    Loop = " << (state.looping ? "true" : "false") << "\n";
        file << "}\n";
    }

    for (const AnimationGraphTransition& transition : graph.transitions)
    {
        file << "\nTransition {\n";
        file << "    From = " << textparse::Quote(transition.fromState) << "\n";
        file << "    To = " << textparse::Quote(transition.toState) << "\n";
        file << "    BlendTime = " << transition.blendTime << "\n";
        file << "    ExitTime = " << transition.exitTime << "\n";
        file << "}\n";

        for (const AnimationGraphCondition& condition : transition.conditions)
        {
            file << "\nCondition {\n";
            file << "    From = " << textparse::Quote(transition.fromState) << "\n";
            file << "    To = " << textparse::Quote(transition.toState) << "\n";
            file << "    Parameter = " << textparse::Quote(condition.parameter) << "\n";
            file << "    Type = " << textparse::Quote(GetConditionTypeName(condition.type)) << "\n";
            file << "    Threshold = " << condition.threshold << "\n";
            file << "}\n";
        }
    }

    return true;
}

#include "anim_blend_tree.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include "kmodel.h"
#include "text_block_file.h"
#include "text_parse_utils.h"

enum class AnimBlendTreeInstance::Kind
{
    Output,
    BindPose,
    Clip,
    Blend,
    Blend1D,
    Additive,
    LayerBlend,
    SelectBool,
    SelectInt,
    FloatParam,
    BoolParam,
    IntParam,
    Math,
    Compare,
    Logic,
    MapRange
};

namespace
{
    using Pose = std::vector<AnimationTransform>;

    AnimNodePortInfo PosePort(const char* name) { return { name, AnimPortType::Pose, 0.0f }; }
    AnimNodePortInfo FloatPort(const char* name, float value = 0.0f) { return { name, AnimPortType::Float, value }; }
    AnimNodePortInfo IntPort(const char* name, float value = 0.0f) { return { name, AnimPortType::Int, value }; }
    AnimNodePortInfo BoolPort(const char* name, float value = 0.0f) { return { name, AnimPortType::Bool, value }; }

    std::vector<AnimNodeInfo> BuildCatalog()
    {
        std::vector<AnimNodeInfo> c;
        c.push_back({ "Output", "Output Pose", "Output", "The final pose of the model. A graph has exactly one.",
                      { PosePort("Pose") }, {}, {} });
        c.push_back({ "BindPose", "Bind Pose", "Pose", "The model's rest pose.", {}, { PosePort("Pose") }, {} });
        c.push_back({ "Clip", "Play Clip", "Pose", "Plays a .kanim clip. Progress is 0..1 through the clip.",
                      { FloatPort("Speed", 1.0f) },
                      { PosePort("Pose"), FloatPort("Progress") },
                      { { "Clip", AnimParamKind::Clip, "", {} }, { "Loop", AnimParamKind::Bool, "true", {} } } });
        c.push_back({ "Blend", "Blend", "Blend", "Mixes A and B. Alpha 0 is all A, 1 is all B.",
                      { PosePort("A"), PosePort("B"), FloatPort("Alpha", 0.5f) }, { PosePort("Pose") }, {} });
        c.push_back({ "Blend1D", "Blend Space 1D", "Blend",
                      "Blends between the connected poses by Value, each placed at its threshold (T0..T3).",
                      { FloatPort("Value"), PosePort("P0"), PosePort("P1"), PosePort("P2"), PosePort("P3") },
                      { PosePort("Pose") },
                      { { "T0", AnimParamKind::Float, "0", {} }, { "T1", AnimParamKind::Float, "1", {} },
                        { "T2", AnimParamKind::Float, "2", {} }, { "T3", AnimParamKind::Float, "3", {} } } });
        c.push_back({ "Additive", "Additive", "Blend",
                      "Adds the difference between Additive and the bind pose on top of Base.",
                      { PosePort("Base"), PosePort("Additive"), FloatPort("Alpha", 1.0f) }, { PosePort("Pose") }, {} });
        c.push_back({ "LayerBlend", "Layer Blend", "Blend",
                      "Blends Layer over Base only on Bone and its children (e.g. upper body).",
                      { PosePort("Base"), PosePort("Layer"), FloatPort("Alpha", 1.0f) }, { PosePort("Pose") },
                      { { "Bone", AnimParamKind::Bone, "", {} } } });
        c.push_back({ "SelectBool", "Select by Bool", "State",
                      "Picks True or False by Condition and cross-fades over BlendTime seconds when it changes.",
                      { PosePort("True"), PosePort("False"), BoolPort("Condition") }, { PosePort("Pose") },
                      { { "BlendTime", AnimParamKind::Float, "0.2", {} } } });
        c.push_back({ "SelectInt", "Select by Int", "State",
                      "Picks P0..P3 by Index and cross-fades over BlendTime seconds when it changes.",
                      { IntPort("Index"), PosePort("P0"), PosePort("P1"), PosePort("P2"), PosePort("P3") },
                      { PosePort("Pose") }, { { "BlendTime", AnimParamKind::Float, "0.2", {} } } });
        c.push_back({ "FloatParam", "Float Parameter", "Parameter", "A float set by game code (SetAnimFloat).",
                      {}, { FloatPort("Value") },
                      { { "Name", AnimParamKind::Name, "Speed", {} }, { "Default", AnimParamKind::Float, "0", {} } } });
        c.push_back({ "BoolParam", "Bool Parameter", "Parameter", "A bool set by game code (SetAnimBool).",
                      {}, { BoolPort("Value") },
                      { { "Name", AnimParamKind::Name, "IsMoving", {} }, { "Default", AnimParamKind::Bool, "false", {} } } });
        c.push_back({ "IntParam", "Int Parameter", "Parameter", "An int set by game code (SetAnimInt).",
                      {}, { IntPort("Value") },
                      { { "Name", AnimParamKind::Name, "State", {} }, { "Default", AnimParamKind::Int, "0", {} } } });
        c.push_back({ "Math", "Math", "Logic", "A op B.", { FloatPort("A"), FloatPort("B") }, { FloatPort("Result") },
                      { { "Op", AnimParamKind::Enum, "Add", { "Add", "Subtract", "Multiply", "Divide", "Min", "Max" } } } });
        c.push_back({ "Compare", "Compare", "Logic", "True when A op B.", { FloatPort("A"), FloatPort("B") },
                      { BoolPort("Result") },
                      { { "Op", AnimParamKind::Enum, "Greater",
                          { "Greater", "Less", "GreaterEqual", "LessEqual", "Equal", "NotEqual" } } } });
        c.push_back({ "Logic", "Logic", "Logic", "A op B on bools. NotA ignores B.", { BoolPort("A"), BoolPort("B") },
                      { BoolPort("Result") }, { { "Op", AnimParamKind::Enum, "And", { "And", "Or", "Xor", "NotA" } } } });
        c.push_back({ "MapRange", "Map Range", "Logic", "Maps Value from In Min..In Max to Out Min..Out Max.",
                      { FloatPort("Value"), FloatPort("InMin", 0.0f), FloatPort("InMax", 1.0f), FloatPort("OutMin", 0.0f),
                        FloatPort("OutMax", 1.0f) },
                      { FloatPort("Result") }, { { "Clamp", AnimParamKind::Bool, "true", {} } } });
        return c;
    }

    float Lerp(float a, float b, float t) { return a + (b - a) * t; }

    void NormalizeQuat(float q[4])
    {
        const float lengthSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (lengthSq <= 1e-12f)
        {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
            return;
        }
        const float inv = 1.0f / std::sqrt(lengthSq);
        for (int i = 0; i < 4; ++i)
            q[i] *= inv;
    }

    void NlerpQuat(const float a[4], const float b[4], float t, float out[4])
    {
        const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        for (int i = 0; i < 4; ++i)
            out[i] = Lerp(a[i], b[i] * sign, t);
        NormalizeQuat(out);
    }

    // (x, y, z, w) quaternions, a then b.
    void MulQuat(const float a[4], const float b[4], float out[4])
    {
        const float r[4] = {
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]
        };
        for (int i = 0; i < 4; ++i)
            out[i] = r[i];
    }

    void BlendTransform(const AnimationTransform& a, const AnimationTransform& b, float t, AnimationTransform& out)
    {
        for (int i = 0; i < 3; ++i)
        {
            out.translation[i] = Lerp(a.translation[i], b.translation[i], t);
            out.scale[i] = Lerp(a.scale[i], b.scale[i], t);
        }
        NlerpQuat(a.rotation, b.rotation, t, out.rotation);
    }

    void BlendPoses(const Pose& a, const Pose& b, float t, Pose& out)
    {
        out.resize(std::min(a.size(), b.size()));
        for (size_t i = 0; i < out.size(); ++i)
            BlendTransform(a[i], b[i], t, out[i]);
    }

    float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }
}

int AnimNodeInfo::InputIndex(const std::string& name) const
{
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        if (inputs[i].name == name)
            return (int)i;
    }
    return -1;
}

int AnimNodeInfo::OutputIndex(const std::string& name) const
{
    for (size_t i = 0; i < outputs.size(); ++i)
    {
        if (outputs[i].name == name)
            return (int)i;
    }
    return -1;
}

const AnimNodeParamInfo* AnimNodeInfo::Param(const std::string& name) const
{
    for (const AnimNodeParamInfo& param : params)
    {
        if (param.name == name)
            return &param;
    }
    return nullptr;
}

const std::vector<AnimNodeInfo>& GetAnimNodeCatalog()
{
    static const std::vector<AnimNodeInfo> catalog = BuildCatalog();
    return catalog;
}

const AnimNodeInfo* FindAnimNodeInfo(const std::string& type)
{
    for (const AnimNodeInfo& info : GetAnimNodeCatalog())
    {
        if (info.type == type)
            return &info;
    }
    return nullptr;
}

const std::string* AnimBlendTreeNode::Find(const std::string& key) const
{
    for (const auto& value : values)
    {
        if (value.first == key)
            return &value.second;
    }
    return nullptr;
}

void AnimBlendTreeNode::Set(const std::string& key, const std::string& value)
{
    for (auto& existing : values)
    {
        if (existing.first == key)
        {
            existing.second = value;
            return;
        }
    }
    values.emplace_back(key, value);
}

bool IsAnimBlendTreePath(const std::string& path)
{
    static const std::string extension = ".kanimgraph";
    if (path.size() < extension.size())
        return false;
    return std::equal(extension.rbegin(), extension.rend(), path.rbegin(),
                      [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); });
}

bool LoadAnimBlendTreeFile(const char* path, AnimBlendTreeData& outTree, std::string* outError)
{
    outTree = {};
    std::vector<TextBlock> blocks;
    if (!LoadTextBlockFile(path, blocks, outError))
        return false;

    for (const TextBlock& block : blocks)
    {
        if (block.type == "AnimBlendTree")
        {
            if (const std::string* value = FindTextBlockProperty(block, "Name"))
                outTree.name = textparse::ParseString(*value);
            if (const std::string* value = FindTextBlockProperty(block, "Model"))
                outTree.modelPath = textparse::ParseString(*value);
        }
        else if (block.type == "Node")
        {
            AnimBlendTreeNode node;
            for (const TextBlockProperty& property : block.properties)
            {
                if (property.key == "Id")
                    node.id = (int32_t)textparse::ParseFloat(property.value, 0.0f);
                else if (property.key == "Type")
                    node.type = textparse::ParseString(property.value);
                else if (property.key == "Position")
                {
                    const Vector2 position = textparse::ParseVector2(property.value, {});
                    node.position[0] = position.x;
                    node.position[1] = position.y;
                }
                else
                    node.values.emplace_back(property.key, textparse::ParseString(property.value));
            }
            outTree.nodes.push_back(std::move(node));
        }
        else if (block.type == "Link")
        {
            AnimBlendTreeLink link;
            if (const std::string* value = FindTextBlockProperty(block, "From"))
                link.fromNode = (int32_t)textparse::ParseFloat(*value, 0.0f);
            if (const std::string* value = FindTextBlockProperty(block, "FromPort"))
                link.fromPort = textparse::ParseString(*value);
            if (const std::string* value = FindTextBlockProperty(block, "To"))
                link.toNode = (int32_t)textparse::ParseFloat(*value, 0.0f);
            if (const std::string* value = FindTextBlockProperty(block, "ToPort"))
                link.toPort = textparse::ParseString(*value);
            outTree.links.push_back(std::move(link));
        }
    }
    return true;
}

bool SaveAnimBlendTreeFile(const char* path, const AnimBlendTreeData& tree, std::string* outError)
{
    const std::filesystem::path outputPath(path ? path : "");
    if (outputPath.empty())
    {
        if (outError)
            *outError = "Missing animation graph output path";
        return false;
    }
    std::error_code ec;
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path(), ec);

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open animation graph file for save";
        return false;
    }

    file << "AnimBlendTree {\n";
    file << "    Version = 1\n";
    if (!tree.name.empty())
        file << "    Name = " << textparse::Quote(tree.name) << "\n";
    if (!tree.modelPath.empty())
        file << "    Model = " << textparse::Quote(tree.modelPath) << "\n";
    file << "}\n";

    for (const AnimBlendTreeNode& node : tree.nodes)
    {
        file << "\nNode {\n";
        file << "    Id = " << node.id << "\n";
        file << "    Type = " << textparse::Quote(node.type) << "\n";
        file << "    Position = \"" << node.position[0] << "," << node.position[1] << "\"\n";
        for (const auto& value : node.values)
            file << "    " << value.first << " = " << textparse::Quote(value.second) << "\n";
        file << "}\n";
    }

    for (const AnimBlendTreeLink& link : tree.links)
    {
        file << "\nLink {\n";
        file << "    From = " << link.fromNode << "\n";
        file << "    FromPort = " << textparse::Quote(link.fromPort) << "\n";
        file << "    To = " << link.toNode << "\n";
        file << "    ToPort = " << textparse::Quote(link.toPort) << "\n";
        file << "}\n";
    }

    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write animation graph";
        return false;
    }
    return true;
}

bool AnimBlendTreeInstance::Build(const AnimBlendTreeData& tree, std::string* outError)
{
    *this = AnimBlendTreeInstance();
    const auto fail = [&](const std::string& message)
    {
        if (outError)
            *outError = message;
        *this = AnimBlendTreeInstance();
        return false;
    };

    static const std::unordered_map<std::string, Kind> kinds = {
        { "Output", Kind::Output }, { "BindPose", Kind::BindPose }, { "Clip", Kind::Clip }, { "Blend", Kind::Blend },
        { "Blend1D", Kind::Blend1D }, { "Additive", Kind::Additive }, { "LayerBlend", Kind::LayerBlend },
        { "SelectBool", Kind::SelectBool }, { "SelectInt", Kind::SelectInt }, { "FloatParam", Kind::FloatParam },
        { "BoolParam", Kind::BoolParam }, { "IntParam", Kind::IntParam }, { "Math", Kind::Math },
        { "Compare", Kind::Compare }, { "Logic", Kind::Logic }, { "MapRange", Kind::MapRange },
    };

    std::unordered_map<int32_t, int32_t> indexById;
    for (const AnimBlendTreeNode& source : tree.nodes)
    {
        const AnimNodeInfo* info = FindAnimNodeInfo(source.type);
        const auto kind = kinds.find(source.type);
        if (!info || kind == kinds.end())
            return fail("Unknown animation graph node type '" + source.type + "'");
        if (indexById.count(source.id))
            return fail("Duplicate animation graph node id " + std::to_string(source.id));

        Node node;
        node.id = source.id;
        node.kind = kind->second;
        const auto text = [&](const std::string& key)
        {
            if (const std::string* value = source.Find(key))
                return *value;
            const AnimNodeParamInfo* param = info->Param(key);
            return param ? param->defaultValue : std::string();
        };
        const auto number = [&](const std::string& key) { return textparse::ParseFloat(text(key), 0.0f); };

        node.inputs.resize(info->inputs.size());
        for (size_t i = 0; i < info->inputs.size(); ++i)
        {
            const AnimNodePortInfo& port = info->inputs[i];
            node.inputs[i].value = port.defaultValue;
            if (const std::string* value = source.Find(port.name))
                node.inputs[i].value = port.type == AnimPortType::Bool ? (textparse::ParseBool(*value, false) ? 1.0f : 0.0f)
                                                                       : textparse::ParseFloat(*value, port.defaultValue);
        }

        for (size_t i = 0; i < info->params.size() && i < 4; ++i)
        {
            const AnimNodeParamInfo& param = info->params[i];
            if (param.kind == AnimParamKind::Float || param.kind == AnimParamKind::Int)
                node.params[i] = number(param.name);
            else if (param.kind == AnimParamKind::Bool)
                node.params[i] = textparse::ParseBool(text(param.name), false) ? 1.0f : 0.0f;
            else if (param.kind == AnimParamKind::Enum)
            {
                const std::string chosen = text(param.name);
                const auto found = std::find(param.options.begin(), param.options.end(), chosen);
                node.op = found == param.options.end() ? 0 : (int)(found - param.options.begin());
            }
            else
                node.text = text(param.name);
        }
        node.flag = node.kind == Kind::Clip ? node.params[1] > 0.5f : node.params[0] > 0.5f;

        if (node.kind == Kind::Output)
        {
            if (outputNode >= 0)
                return fail("Animation graph has more than one Output node");
            outputNode = (int32_t)nodes.size();
        }

        if (node.kind == Kind::FloatParam || node.kind == Kind::BoolParam || node.kind == Kind::IntParam)
        {
            const AnimPortType type = node.kind == Kind::FloatParam ? AnimPortType::Float
                                      : node.kind == Kind::BoolParam ? AnimPortType::Bool
                                                                     : AnimPortType::Int;
            const bool known = std::any_of(parameters.begin(), parameters.end(),
                                           [&](const AnimBlendTreeParameter& p) { return p.name == node.text; });
            if (!node.text.empty() && !known)
                parameters.push_back({ node.text, type, node.params[1] });
        }

        indexById[node.id] = (int32_t)nodes.size();
        nodes.push_back(std::move(node));
    }

    if (outputNode < 0)
        return fail("Animation graph has no Output node");

    for (const AnimBlendTreeLink& link : tree.links)
    {
        const auto from = indexById.find(link.fromNode);
        const auto to = indexById.find(link.toNode);
        if (from == indexById.end() || to == indexById.end())
            return fail("Animation graph link references a missing node");
        const AnimNodeInfo* fromInfo = FindAnimNodeInfo(tree.nodes[(size_t)from->second].type);
        const AnimNodeInfo* toInfo = FindAnimNodeInfo(tree.nodes[(size_t)to->second].type);
        const int fromPort = fromInfo->OutputIndex(link.fromPort);
        const int toPort = toInfo->InputIndex(link.toPort);
        if (fromPort < 0 || toPort < 0)
            return fail("Animation graph link references a missing port");
        const bool fromPose = fromInfo->outputs[(size_t)fromPort].type == AnimPortType::Pose;
        const bool toPose = toInfo->inputs[(size_t)toPort].type == AnimPortType::Pose;
        if (fromPose != toPose)
            return fail("Animation graph link connects a pose to a value");
        nodes[(size_t)to->second].inputs[(size_t)toPort].node = from->second;
        nodes[(size_t)to->second].inputs[(size_t)toPort].port = fromPort;
    }

    // Links only point back along evaluation, so a cycle would recurse forever.
    std::vector<uint8_t> state(nodes.size(), 0);
    std::function<bool(int32_t)> visit = [&](int32_t index)
    {
        if (state[(size_t)index] == 1)
            return false;
        if (state[(size_t)index] == 2)
            return true;
        state[(size_t)index] = 1;
        for (const Input& input : nodes[(size_t)index].inputs)
        {
            if (input.node >= 0 && !visit(input.node))
                return false;
        }
        state[(size_t)index] = 2;
        return true;
    };
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        if (!visit((int32_t)i))
            return fail("Animation graph has a cycle");
    }

    Reset();
    return true;
}

void AnimBlendTreeInstance::Reset()
{
    values.clear();
    for (const AnimBlendTreeParameter& parameter : parameters)
        values[parameter.name] = parameter.defaultValue;
    for (Node& node : nodes)
    {
        node.stamp = 0;
        node.time = 0.0f;
        node.progress = 0.0f;
        node.active = -1;
        node.previous = -1;
        node.fade = 1.0f;
        node.weight = 0.0f;
    }
    frame = 0;
}

void AnimBlendTreeInstance::SetFloat(const std::string& name, float value)
{
    values[name] = value;
}

void AnimBlendTreeInstance::SetBool(const std::string& name, bool value)
{
    values[name] = value ? 1.0f : 0.0f;
}

void AnimBlendTreeInstance::SetInt(const std::string& name, int value)
{
    values[name] = (float)value;
}

float AnimBlendTreeInstance::GetValue(const std::string& name) const
{
    const auto found = values.find(name);
    return found == values.end() ? 0.0f : found->second;
}

float AnimBlendTreeInstance::GetNodeTime(int32_t nodeId) const
{
    for (const Node& node : nodes)
    {
        if (node.id == nodeId)
            return node.time;
    }
    return 0.0f;
}

float AnimBlendTreeInstance::GetNodeWeight(int32_t nodeId) const
{
    for (const Node& node : nodes)
    {
        if (node.id == nodeId)
            return node.stamp == frame ? node.weight : 0.0f;
    }
    return 0.0f;
}

bool AnimBlendTreeInstance::Evaluate(const KModelData& evalModel, float dt, const ClipResolver& resolver, std::vector<AnimationTransform>& outPose)
{
    if (outputNode < 0)
        return false;

    if (bindPoseModel != &evalModel || bindPose.size() != evalModel.nodes.size())
    {
        BuildAnimationBindPose(evalModel, bindPose);
        bindPoseModel = &evalModel;
    }
    model = &evalModel;
    clips = &resolver;
    frameDt = dt;
    if (++frame == 0)
        frame = 1;

    outPose = InputPose(nodes[(size_t)outputNode], 0, 1.0f);
    nodes[(size_t)outputNode].stamp = frame;
    nodes[(size_t)outputNode].weight = 1.0f;
    model = nullptr;
    clips = nullptr;
    return true;
}

float AnimBlendTreeInstance::InputScalar(const Node& node, size_t input)
{
    const Input& in = node.inputs[input];
    return in.node >= 0 ? EvalScalar(in.node, in.port) : in.value;
}

const std::vector<AnimationTransform>& AnimBlendTreeInstance::InputPose(const Node& node, size_t input, float weight)
{
    const Input& in = node.inputs[input];
    return in.node >= 0 ? EvalPose(in.node, weight) : bindPose;
}

float AnimBlendTreeInstance::EvalScalar(int32_t nodeIndex, int32_t port)
{
    Node& node = nodes[(size_t)nodeIndex];
    if (node.kind == Kind::Clip)
    {
        EvalPose(nodeIndex, 0.0f);
        return node.progress;
    }
    (void)port;
    if (node.stamp == frame)
        return node.scalar;

    float result = 0.0f;
    switch (node.kind)
    {
    case Kind::FloatParam:
    case Kind::BoolParam:
    case Kind::IntParam:
    {
        const auto found = values.find(node.text);
        result = found != values.end() ? found->second : node.params[1];
        break;
    }
    case Kind::Math:
    {
        const float a = InputScalar(node, 0);
        const float b = InputScalar(node, 1);
        switch (node.op)
        {
        case 0: result = a + b; break;
        case 1: result = a - b; break;
        case 2: result = a * b; break;
        case 3: result = std::fabs(b) > 1e-8f ? a / b : 0.0f; break;
        case 4: result = std::min(a, b); break;
        default: result = std::max(a, b); break;
        }
        break;
    }
    case Kind::Compare:
    {
        const float a = InputScalar(node, 0);
        const float b = InputScalar(node, 1);
        bool r = false;
        switch (node.op)
        {
        case 0: r = a > b; break;
        case 1: r = a < b; break;
        case 2: r = a >= b; break;
        case 3: r = a <= b; break;
        case 4: r = std::fabs(a - b) <= 1e-5f; break;
        default: r = std::fabs(a - b) > 1e-5f; break;
        }
        result = r ? 1.0f : 0.0f;
        break;
    }
    case Kind::Logic:
    {
        const bool a = InputScalar(node, 0) > 0.5f;
        const bool b = InputScalar(node, 1) > 0.5f;
        bool r = false;
        switch (node.op)
        {
        case 0: r = a && b; break;
        case 1: r = a || b; break;
        case 2: r = a != b; break;
        default: r = !a; break;
        }
        result = r ? 1.0f : 0.0f;
        break;
    }
    case Kind::MapRange:
    {
        const float value = InputScalar(node, 0);
        const float inMin = InputScalar(node, 1);
        const float inMax = InputScalar(node, 2);
        const float outMin = InputScalar(node, 3);
        const float outMax = InputScalar(node, 4);
        float t = std::fabs(inMax - inMin) > 1e-8f ? (value - inMin) / (inMax - inMin) : 0.0f;
        if (node.flag)
            t = Clamp01(t);
        result = Lerp(outMin, outMax, t);
        break;
    }
    default:
        break;
    }

    node.scalar = result;
    node.stamp = frame;
    node.weight = 0.0f;
    return result;
}

void AnimBlendTreeInstance::EvalSelect(Node& node, int32_t target, size_t firstPose, size_t poseCount, float weight)
{
    target = std::clamp(target, 0, (int32_t)poseCount - 1);
    const float blendTime = std::max(node.params[0], 0.0f);
    if (node.active < 0)
    {
        node.active = target;
        node.fade = 1.0f;
    }
    else if (target != node.active)
    {
        node.previous = node.active;
        node.active = target;
        node.fade = 0.0f;
    }
    else if (node.fade < 1.0f)
    {
        node.fade = blendTime > 0.0f ? Clamp01(node.fade + frameDt / blendTime) : 1.0f;
    }
    if (blendTime <= 0.0f)
        node.fade = 1.0f;

    if (node.fade >= 1.0f || node.previous < 0)
    {
        node.pose = InputPose(node, firstPose + (size_t)node.active, weight);
        return;
    }
    const Pose& from = InputPose(node, firstPose + (size_t)node.previous, weight * (1.0f - node.fade));
    const Pose& to = InputPose(node, firstPose + (size_t)node.active, weight * node.fade);
    BlendPoses(from, to, node.fade, node.pose);
}

const std::vector<AnimationTransform>& AnimBlendTreeInstance::EvalPose(int32_t nodeIndex, float weight)
{
    Node& node = nodes[(size_t)nodeIndex];
    if (node.stamp == frame)
    {
        node.weight += weight;
        return node.pose;
    }
    node.stamp = frame;
    node.weight = weight;

    switch (node.kind)
    {
    case Kind::Clip:
    {
        if (!node.clipResolved)
        {
            node.clipResolved = true;
            node.clip = (clips && *clips && !node.text.empty()) ? (*clips)(node.text) : nullptr;
        }
        if (!node.clip)
        {
            node.pose = bindPose;
            node.progress = 0.0f;
            break;
        }
        const float duration = node.clip->duration;
        node.time += frameDt * InputScalar(node, 0);
        if (node.flag && duration > 0.0f)
        {
            node.time = std::fmod(node.time, duration);
            if (node.time < 0.0f)
                node.time += duration;
        }
        else
            node.time = std::clamp(node.time, 0.0f, std::max(duration, 0.0f));
        node.progress = duration > 0.0f ? Clamp01(node.time / duration) : 1.0f;

        // A looping clip file wraps its own time, so hold a played-once clip just short of the end.
        const float sampleTime = node.flag ? node.time : std::min(node.time, std::max(duration - 1e-4f, 0.0f));
        SampleAnimationClip(*model, *node.clip, sampleTime, node.pose);
        break;
    }
    case Kind::Blend:
    {
        const float alpha = Clamp01(InputScalar(node, 2));
        const Pose& a = InputPose(node, 0, weight * (1.0f - alpha));
        const Pose& b = InputPose(node, 1, weight * alpha);
        BlendPoses(a, b, alpha, node.pose);
        break;
    }
    case Kind::Blend1D:
    {
        const float value = InputScalar(node, 0);
        struct Point
        {
            float threshold;
            size_t input;
        };
        std::vector<Point> points;
        for (size_t i = 0; i < 4; ++i)
        {
            if (node.inputs[i + 1].node >= 0)
                points.push_back({ node.params[i], i + 1 });
        }
        if (points.empty())
        {
            node.pose = bindPose;
            break;
        }
        std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.threshold < b.threshold; });
        if (value <= points.front().threshold || points.size() == 1)
        {
            node.pose = InputPose(node, points.front().input, weight);
            break;
        }
        if (value >= points.back().threshold)
        {
            node.pose = InputPose(node, points.back().input, weight);
            break;
        }
        size_t upper = 1;
        while (upper < points.size() && points[upper].threshold < value)
            ++upper;
        const Point& lo = points[upper - 1];
        const Point& hi = points[upper];
        const float span = hi.threshold - lo.threshold;
        const float t = span > 1e-8f ? (value - lo.threshold) / span : 0.0f;
        const Pose& a = InputPose(node, lo.input, weight * (1.0f - t));
        const Pose& b = InputPose(node, hi.input, weight * t);
        BlendPoses(a, b, t, node.pose);
        break;
    }
    case Kind::Additive:
    {
        const float alpha = InputScalar(node, 2);
        const Pose& base = InputPose(node, 0, weight);
        const Pose& additive = InputPose(node, 1, weight * Clamp01(alpha));
        node.pose = base;
        const size_t count = std::min({ base.size(), additive.size(), bindPose.size() });
        for (size_t i = 0; i < count; ++i)
        {
            const AnimationTransform& ref = bindPose[i];
            const AnimationTransform& add = additive[i];
            AnimationTransform& out = node.pose[i];
            for (int k = 0; k < 3; ++k)
            {
                out.translation[k] += alpha * (add.translation[k] - ref.translation[k]);
                const float refScale = std::fabs(ref.scale[k]) > 1e-6f ? ref.scale[k] : 1.0f;
                out.scale[k] *= 1.0f + alpha * (add.scale[k] / refScale - 1.0f);
            }
            const float inverseRef[4] = { -ref.rotation[0], -ref.rotation[1], -ref.rotation[2], ref.rotation[3] };
            float delta[4];
            MulQuat(inverseRef, add.rotation, delta);
            const float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            float scaled[4];
            NlerpQuat(identity, delta, alpha, scaled);
            MulQuat(out.rotation, scaled, out.rotation);
            NormalizeQuat(out.rotation);
        }
        break;
    }
    case Kind::LayerBlend:
    {
        const float alpha = Clamp01(InputScalar(node, 2));
        const Pose& base = InputPose(node, 0, weight);
        const Pose& layer = InputPose(node, 1, weight * alpha);
        if (node.maskModel != model)
        {
            node.maskModel = model;
            const size_t count = model->nodes.size();
            node.mask.assign(count, node.text.empty() ? 1 : 0);
            int32_t root = -1;
            for (size_t i = 0; i < count; ++i)
            {
                if (model->nodes[i].name == node.text)
                    root = (int32_t)i;
            }
            for (size_t i = 0; root >= 0 && i < count; ++i)
            {
                size_t guard = 0;
                for (int32_t walk = (int32_t)i; walk >= 0 && walk < (int32_t)count && guard++ <= count; walk = model->nodes[(size_t)walk].parentIndex)
                {
                    if (walk == root)
                    {
                        node.mask[i] = 1;
                        break;
                    }
                }
            }
        }
        node.pose = base;
        const size_t count = std::min({ base.size(), layer.size(), node.mask.size() });
        for (size_t i = 0; i < count; ++i)
        {
            if (node.mask[i])
                BlendTransform(base[i], layer[i], alpha, node.pose[i]);
        }
        break;
    }
    case Kind::SelectBool:
        EvalSelect(node, InputScalar(node, 2) > 0.5f ? 0 : 1, 0, 2, weight);
        break;
    case Kind::SelectInt:
        EvalSelect(node, (int32_t)std::lround(InputScalar(node, 0)), 1, 4, weight);
        break;
    case Kind::BindPose:
    default:
        node.pose = bindPose;
        break;
    }
    return node.pose;
}

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "animation_clip.h"

// Pose blend tree (.kanimgraph): a node graph from clips, blends and parameter
// logic to one Output pose. The model editor (tools/modelEditor) compiles this
// file as well, so its preview runs the same evaluator as the game.

struct KModelData;
using AnimBlendTreeHandle = uint32_t;

enum class AnimPortType
{
    Pose,
    Float,
    Int,
    Bool
};

enum class AnimParamKind
{
    Float,
    Int,
    Bool,
    Enum,
    Name,   // graph parameter name
    Clip,   // .kanim path
    Bone    // node name in the model
};

struct AnimNodePortInfo
{
    std::string name;
    AnimPortType type = AnimPortType::Float;
    float defaultValue = 0.0f;
};

struct AnimNodeParamInfo
{
    std::string name;
    AnimParamKind kind = AnimParamKind::Float;
    std::string defaultValue;
    std::vector<std::string> options;
};

struct AnimNodeInfo
{
    std::string type;
    std::string title;
    std::string category;
    std::string description;
    std::vector<AnimNodePortInfo> inputs;
    std::vector<AnimNodePortInfo> outputs;
    std::vector<AnimNodeParamInfo> params;

    int InputIndex(const std::string& name) const;
    int OutputIndex(const std::string& name) const;
    const AnimNodeParamInfo* Param(const std::string& name) const;
};

const std::vector<AnimNodeInfo>& GetAnimNodeCatalog();
const AnimNodeInfo* FindAnimNodeInfo(const std::string& type);

struct AnimBlendTreeNode
{
    int32_t id = 0;
    std::string type;
    float position[2] = { 0.0f, 0.0f };
    // Parameter values and values of unconnected inputs, as text.
    std::vector<std::pair<std::string, std::string>> values;

    const std::string* Find(const std::string& key) const;
    void Set(const std::string& key, const std::string& value);
};

struct AnimBlendTreeLink
{
    int32_t fromNode = 0;
    std::string fromPort;
    int32_t toNode = 0;
    std::string toPort;
};

struct AnimBlendTreeData
{
    std::string name;
    std::string modelPath;
    std::vector<AnimBlendTreeNode> nodes;
    std::vector<AnimBlendTreeLink> links;
};

bool IsAnimBlendTreePath(const std::string& path);
bool LoadAnimBlendTreeFile(const char* path, AnimBlendTreeData& outTree, std::string* outError = nullptr);
bool SaveAnimBlendTreeFile(const char* path, const AnimBlendTreeData& tree, std::string* outError = nullptr);

struct AnimBlendTreeParameter
{
    std::string name;
    AnimPortType type = AnimPortType::Float;
    float defaultValue = 0.0f;
};

class AnimBlendTreeInstance
{
public:
    using ClipResolver = std::function<const AnimationClipData*(const std::string& path)>;

    // Fails when there is no Output node or a link names a missing node or port.
    bool Build(const AnimBlendTreeData& tree, std::string* outError = nullptr);
    bool IsBuilt() const { return outputNode >= 0; }
    void Reset();

    const std::vector<AnimBlendTreeParameter>& Parameters() const { return parameters; }
    void SetFloat(const std::string& name, float value);
    void SetBool(const std::string& name, bool value);
    void SetInt(const std::string& name, int value);
    float GetValue(const std::string& name) const;

    bool Evaluate(const KModelData& model, float dt, const ClipResolver& clips, std::vector<AnimationTransform>& outPose);

    // For editor display: clip time of a Clip node, weight 0..1 the Output gave it last frame.
    float GetNodeTime(int32_t nodeId) const;
    float GetNodeWeight(int32_t nodeId) const;

private:
    enum class Kind;
    struct Input
    {
        int32_t node = -1;
        int32_t port = 0;
        float value = 0.0f;
    };
    struct Node
    {
        int32_t id = 0;
        Kind kind = {};
        std::vector<Input> inputs;
        float params[4] = {};
        int op = 0;
        bool flag = false;
        std::string text;

        uint32_t stamp = 0;
        float scalar = 0.0f;
        float weight = 0.0f;
        std::vector<AnimationTransform> pose;
        float time = 0.0f;
        float progress = 0.0f;
        int32_t active = -1;
        int32_t previous = -1;
        float fade = 1.0f;
        const AnimationClipData* clip = nullptr;
        bool clipResolved = false;
        std::vector<uint8_t> mask;
        const KModelData* maskModel = nullptr;
    };

    float EvalScalar(int32_t nodeIndex, int32_t port);
    float InputScalar(const Node& node, size_t input);
    const std::vector<AnimationTransform>& InputPose(const Node& node, size_t input, float weight);
    const std::vector<AnimationTransform>& EvalPose(int32_t nodeIndex, float weight);
    void EvalSelect(Node& node, int32_t target, size_t firstPose, size_t poseCount, float weight);

    std::vector<Node> nodes;
    std::vector<AnimBlendTreeParameter> parameters;
    std::unordered_map<std::string, float> values;
    int32_t outputNode = -1;
    uint32_t frame = 0;
    float frameDt = 0.0f;
    const KModelData* model = nullptr;
    const ClipResolver* clips = nullptr;
    std::vector<AnimationTransform> bindPose;
    const KModelData* bindPoseModel = nullptr;
};

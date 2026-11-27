#include "test_framework.h"
#include <fstream>
#include "anim_blend_tree.h"
#include "animation_pose.h"
#include "kmodel.h"

namespace
{
    KModelData MakeTwoNodeModel()
    {
        KModelData model;
        KModelNode root;
        root.name = "root";
        model.nodes.push_back(root);
        KModelNode arm;
        arm.name = "arm";
        arm.parentIndex = 0;
        arm.translation[1] = 1.0f;
        model.nodes.push_back(arm);
        return model;
    }

    // Moves `node` along X from 0 to `distance` over one second.
    AnimationClipData MakeSlide(int32_t node, float distance)
    {
        AnimationClipData clip;
        clip.duration = 1.0f;
        AnimationNodeTrack track;
        track.nodeIndex = node;
        track.translationKeys = { { 0.0f, { 0.0f, 0.0f, 0.0f } }, { 1.0f, { distance, 0.0f, 0.0f } } };
        clip.tracks.push_back(track);
        return clip;
    }

    AnimBlendTreeNode MakeNode(int32_t id, const char* type, std::vector<std::pair<std::string, std::string>> values = {})
    {
        AnimBlendTreeNode node;
        node.id = id;
        node.type = type;
        node.values = std::move(values);
        return node;
    }
}

TEST_CASE("anim graph: file round trip keeps nodes, values and links")
{
    kestest::TempDir dir;
    const std::string path = dir.File("test.ag");

    AnimBlendTreeData tree;
    tree.modelPath = "meshes/guy.kmodel";
    tree.nodes.push_back(MakeNode(1, "Clip", { { "Clip", "anims/walk \"fast\".kanim" }, { "Speed", "1.5" } }));
    tree.nodes.push_back(MakeNode(2, "Output"));
    tree.nodes[0].position[0] = 120.0f;
    tree.links.push_back({ 1, "Pose", 2, "Pose" });
    REQUIRE(SaveAnimBlendTreeFile(path.c_str(), tree));

    AnimBlendTreeData loaded;
    std::string error;
    REQUIRE(LoadAnimBlendTreeFile(path.c_str(), loaded, &error));
    CHECK(loaded.modelPath == "meshes/guy.kmodel");
    REQUIRE(loaded.nodes.size() == 2);
    CHECK(loaded.nodes[0].type == "Clip");
    CHECK_NEAR(loaded.nodes[0].position[0], 120.0, 1e-4);
    REQUIRE(loaded.nodes[0].Find("Clip") != nullptr);
    CHECK(*loaded.nodes[0].Find("Clip") == "anims/walk \"fast\".kanim");
    REQUIRE(loaded.links.size() == 1);
    CHECK(loaded.links[0].toPort == "Pose");
    CHECK(IsAnimBlendTreePath(path));
    CHECK(!IsAnimBlendTreePath("graphs/old.animgraph"));
}

TEST_CASE("anim graph: build rejects broken graphs")
{
    AnimBlendTreeInstance instance;
    AnimBlendTreeData tree;
    tree.nodes.push_back(MakeNode(1, "BindPose"));
    CHECK(!instance.Build(tree));

    tree.nodes.push_back(MakeNode(2, "Output"));
    tree.links.push_back({ 1, "Pose", 2, "Pose" });
    CHECK(instance.Build(tree));

    tree.links.push_back({ 9, "Pose", 2, "Pose" });
    CHECK(!instance.Build(tree));
    tree.links.pop_back();

    tree.nodes.push_back(MakeNode(3, "FloatParam"));
    tree.links.push_back({ 3, "Value", 2, "Pose" });
    std::string error;
    CHECK(!instance.Build(tree, &error));
    CHECK(!error.empty());
    tree.links.pop_back();

    tree.nodes.push_back(MakeNode(4, "Math"));
    tree.nodes.push_back(MakeNode(5, "Math"));
    tree.links.push_back({ 4, "Result", 5, "A" });
    tree.links.push_back({ 5, "Result", 4, "A" });
    CHECK(!instance.Build(tree));
}

TEST_CASE("anim graph: blend by parameter and clip time advance")
{
    const KModelData model = MakeTwoNodeModel();
    const AnimationClipData slideA = MakeSlide(1, 2.0f);
    const AnimationClipData slideB = MakeSlide(1, -2.0f);

    AnimBlendTreeData tree;
    tree.nodes.push_back(MakeNode(1, "Clip", { { "Clip", "a" } }));
    tree.nodes.push_back(MakeNode(2, "Clip", { { "Clip", "b" } }));
    tree.nodes.push_back(MakeNode(3, "FloatParam", { { "Name", "Mix" }, { "Default", "0" } }));
    tree.nodes.push_back(MakeNode(4, "Blend"));
    tree.nodes.push_back(MakeNode(5, "Output"));
    tree.links = { { 1, "Pose", 4, "A" }, { 2, "Pose", 4, "B" }, { 3, "Value", 4, "Alpha" }, { 4, "Pose", 5, "Pose" } };

    AnimBlendTreeInstance instance;
    std::string error;
    REQUIRE(instance.Build(tree, &error));
    REQUIRE(instance.Parameters().size() == 1);
    CHECK(instance.Parameters()[0].name == "Mix");

    const AnimBlendTreeInstance::ClipResolver clips = [&](const std::string& path) -> const AnimationClipData*
    { return path == "a" ? &slideA : path == "b" ? &slideB
                                                 : nullptr; };

    std::vector<AnimationTransform> pose;
    REQUIRE(instance.Evaluate(model, 0.5f, clips, pose));
    REQUIRE(pose.size() == 2);
    CHECK_NEAR(pose[1].translation[0], 1.0, 1e-4);
    CHECK_NEAR(instance.GetNodeTime(1), 0.5, 1e-5);
    CHECK_NEAR(instance.GetNodeWeight(1), 1.0, 1e-5);
    CHECK_NEAR(instance.GetNodeWeight(2), 0.0, 1e-5);

    instance.SetFloat("Mix", 0.5f);
    REQUIRE(instance.Evaluate(model, 0.0f, clips, pose));
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-4);

    instance.SetFloat("Mix", 1.0f);
    REQUIRE(instance.Evaluate(model, 0.25f, clips, pose));
    CHECK_NEAR(pose[1].translation[0], -1.5, 1e-4);
}

TEST_CASE("anim graph: select cross-fades and layer blend masks children")
{
    const KModelData model = MakeTwoNodeModel();
    const AnimationClipData slide = MakeSlide(1, 2.0f);
    const AnimBlendTreeInstance::ClipResolver clips = [&](const std::string&)
    { return &slide; };

    AnimBlendTreeData tree;
    tree.nodes.push_back(MakeNode(1, "Clip", { { "Clip", "slide" } }));
    tree.nodes.push_back(MakeNode(2, "BindPose"));
    tree.nodes.push_back(MakeNode(3, "BoolParam", { { "Name", "Moving" } }));
    tree.nodes.push_back(MakeNode(4, "SelectBool", { { "BlendTime", "1" } }));
    tree.nodes.push_back(MakeNode(5, "Output"));
    tree.links = { { 1, "Pose", 4, "True" }, { 2, "Pose", 4, "False" }, { 3, "Value", 4, "Condition" }, { 4, "Pose", 5, "Pose" } };

    AnimBlendTreeInstance instance;
    REQUIRE(instance.Build(tree));
    std::vector<AnimationTransform> pose;
    REQUIRE(instance.Evaluate(model, 0.0f, clips, pose));
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-5);

    instance.SetBool("Moving", true);
    REQUIRE(instance.Evaluate(model, 0.5f, clips, pose)); // the switch frame starts the fade at 0
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-5);
    REQUIRE(instance.Evaluate(model, 0.25f, clips, pose)); // clip at 0.75 s (x 1.5), a quarter faded in
    CHECK_NEAR(pose[1].translation[0], 0.375, 1e-4);

    AnimBlendTreeData layer;
    layer.nodes.push_back(MakeNode(1, "Clip", { { "Clip", "slide" } }));
    layer.nodes.push_back(MakeNode(2, "BindPose"));
    layer.nodes.push_back(MakeNode(3, "LayerBlend", { { "Bone", "root" } }));
    layer.nodes.push_back(MakeNode(4, "Output"));
    layer.links = { { 2, "Pose", 3, "Base" }, { 1, "Pose", 3, "Layer" }, { 3, "Pose", 4, "Pose" } };
    REQUIRE(instance.Build(layer));
    REQUIRE(instance.Evaluate(model, 0.5f, clips, pose));
    CHECK_NEAR(pose[1].translation[0], 1.0, 1e-4);

    layer.nodes[2].Set("Bone", "nothing");
    REQUIRE(instance.Build(layer));
    REQUIRE(instance.Evaluate(model, 0.5f, clips, pose));
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-4);
}

namespace
{
    AnimBlendTreeNode MakeStateNode(int32_t id, int32_t graph, const char* type, std::vector<std::pair<std::string, std::string>> values = {})
    {
        AnimBlendTreeNode node = MakeNode(id, type, std::move(values));
        node.graph = graph;
        return node;
    }

    AnimBlendTreeState MakeState(int32_t id, int32_t machine, const char* name, bool entry = false)
    {
        AnimBlendTreeState state;
        state.id = id;
        state.machine = machine;
        state.name = name;
        state.entry = entry;
        return state;
    }

    AnimBlendTreeTransition MakeTransition(int32_t machine, int32_t from, int32_t to, float duration,
        std::vector<AnimTransitionCondition> conditions, float exitTime = -1.0f)
    {
        AnimBlendTreeTransition transition;
        transition.machine = machine;
        transition.from = from;
        transition.to = to;
        transition.duration = duration;
        transition.exitTime = exitTime;
        transition.conditions = std::move(conditions);
        return transition;
    }

    // Machine node 1 with Idle (state 10, bind pose) and Walk (state 11, the slide clip).
    AnimBlendTreeData MakeLocomotion()
    {
        AnimBlendTreeData tree;
        tree.nodes.push_back(MakeNode(1, "StateMachine"));
        tree.nodes.push_back(MakeNode(2, "Output"));
        tree.nodes.push_back(MakeStateNode(3, 10, "BindPose"));
        tree.nodes.push_back(MakeStateNode(4, 10, "Output"));
        tree.nodes.push_back(MakeStateNode(5, 11, "Clip", { { "Clip", "slide" } }));
        tree.nodes.push_back(MakeStateNode(6, 11, "Output"));
        tree.links = { { 1, "Pose", 2, "Pose" }, { 3, "Pose", 4, "Pose" }, { 5, "Pose", 6, "Pose" } };
        tree.states = { MakeState(10, 1, "Idle", true), MakeState(11, 1, "Walk") };
        tree.transitions = {
            MakeTransition(1, 10, 11, 1.0f, { { "Speed", AnimConditionOp::Greater, 0.5f } }),
            MakeTransition(1, 11, 10, 0.0f, { { "Stop", AnimConditionOp::Triggered, 0.0f } }),
        };
        return tree;
    }
}

TEST_CASE("anim graph: state machine file round trip")
{
    kestest::TempDir dir;
    const std::string path = dir.File("machine.ag");
    AnimBlendTreeData tree = MakeLocomotion();
    tree.states[1].position[0] = 250.0f;
    tree.transitions.push_back(MakeTransition(1, AnimBlendTreeTransition::AnyState, 10, 0.3f,
        { { "Dead", AnimConditionOp::IsTrue, 0.0f }, { "Hp", AnimConditionOp::LessEqual, 0.25f } }, 0.75f));
    REQUIRE(SaveAnimBlendTreeFile(path.c_str(), tree));

    AnimBlendTreeData loaded;
    std::string error;
    REQUIRE(LoadAnimBlendTreeFile(path.c_str(), loaded, &error));
    REQUIRE(loaded.nodes.size() == tree.nodes.size());
    CHECK(loaded.nodes[0].graph == 0);
    CHECK(loaded.nodes[4].graph == 11);
    REQUIRE(loaded.states.size() == 2);
    CHECK(loaded.states[0].entry);
    CHECK(!loaded.states[1].entry);
    CHECK(loaded.states[1].name == "Walk");
    CHECK_NEAR(loaded.states[1].position[0], 250.0, 1e-4);
    REQUIRE(loaded.transitions.size() == 3);
    const AnimBlendTreeTransition& any = loaded.transitions[2];
    CHECK(any.from == AnimBlendTreeTransition::AnyState);
    CHECK(any.to == 10);
    CHECK_NEAR(any.duration, 0.3, 1e-5);
    CHECK_NEAR(any.exitTime, 0.75, 1e-5);
    REQUIRE(any.conditions.size() == 2);
    CHECK(any.conditions[0].parameter == "Dead");
    CHECK(any.conditions[0].op == AnimConditionOp::IsTrue);
    CHECK(any.conditions[1].op == AnimConditionOp::LessEqual);
    CHECK_NEAR(any.conditions[1].value, 0.25, 1e-5);
    CHECK(loaded.transitions[0].exitTime < 0.0f);

    AnimBlendTreeInstance instance;
    CHECK(instance.Build(loaded, &error));
}

TEST_CASE("anim graph: state machine transitions, cross-fade and triggers")
{
    const KModelData model = MakeTwoNodeModel();
    const AnimationClipData slide = MakeSlide(1, 2.0f);
    const AnimBlendTreeInstance::ClipResolver clips = [&](const std::string&)
    { return &slide; };

    AnimBlendTreeInstance instance;
    std::string error;
    REQUIRE(instance.Build(MakeLocomotion(), &error));
    REQUIRE(instance.Parameters().size() == 2);
    CHECK(instance.Parameters()[0].name == "Speed");
    CHECK(!instance.Parameters()[0].trigger);
    CHECK(instance.Parameters()[1].name == "Stop");
    CHECK(instance.Parameters()[1].trigger);

    std::vector<AnimationTransform> pose;
    REQUIRE(instance.Evaluate(model, 0.5f, clips, pose));
    CHECK(instance.GetActiveState(1) == 10);
    CHECK_NEAR(instance.GetStateWeight(10), 1.0, 1e-5);
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-5);
    CHECK_NEAR(instance.GetNodeWeight(5), 0.0, 1e-5);

    instance.SetFloat("Speed", 1.0f);
    REQUIRE(instance.Evaluate(model, 0.5f, clips, pose)); // the transition frame starts the fade at 0
    CHECK(instance.GetActiveState(1) == 11);
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-5);
    CHECK_NEAR(instance.GetNodeTime(5), 0.5, 1e-5); // entering Walk restarted its clip
    REQUIRE(instance.Evaluate(model, 0.25f, clips, pose)); // clip at 0.75 s (x 1.5), a quarter faded in
    CHECK_NEAR(pose[1].translation[0], 0.375, 1e-4);
    CHECK_NEAR(instance.GetStateWeight(11), 0.25, 1e-5);
    CHECK_NEAR(instance.GetStateWeight(10), 0.75, 1e-5);

    instance.SetFloat("Speed", 0.0f);
    instance.SetTrigger("Stop");
    REQUIRE(instance.Evaluate(model, 0.1f, clips, pose));
    CHECK(instance.GetActiveState(1) == 10);
    CHECK_NEAR(pose[1].translation[0], 0.0, 1e-5);
    CHECK_NEAR(instance.GetValue("Stop"), 0.0, 1e-5);

    instance.Reset();
    REQUIRE(instance.Evaluate(model, 0.1f, clips, pose));
    CHECK(instance.GetActiveState(1) == 10);
}

TEST_CASE("anim graph: state machine exit time and validation")
{
    const KModelData model = MakeTwoNodeModel();
    const AnimationClipData slide = MakeSlide(1, 2.0f);
    const AnimBlendTreeInstance::ClipResolver clips = [&](const std::string&)
    { return &slide; };

    AnimBlendTreeData tree = MakeLocomotion();
    tree.states[0].entry = false;
    tree.states[1].entry = true;
    tree.nodes[4].Set("Loop", "false");
    tree.transitions = { MakeTransition(1, 11, 10, 0.0f, {}, 1.0f), MakeTransition(1, 10, 11, 0.0f, {}) };

    AnimBlendTreeInstance instance;
    std::string error;
    REQUIRE(instance.Build(tree, &error));
    std::vector<AnimationTransform> pose;
    REQUIRE(instance.Evaluate(model, 0.6f, clips, pose));
    CHECK(instance.GetActiveState(1) == 11);
    REQUIRE(instance.Evaluate(model, 0.6f, clips, pose)); // clip reaches its end this frame
    CHECK(instance.GetActiveState(1) == 11);
    REQUIRE(instance.Evaluate(model, 0.1f, clips, pose));
    CHECK(instance.GetActiveState(1) == 10);
    REQUIRE(instance.Evaluate(model, 0.1f, clips, pose)); // no conditions and no exit time never fires
    CHECK(instance.GetActiveState(1) == 10);

    AnimBlendTreeData crossing = MakeLocomotion();
    crossing.links.push_back({ 3, "Pose", 6, "Pose" });
    CHECK(!instance.Build(crossing, &error));

    AnimBlendTreeData missing = MakeLocomotion();
    missing.transitions.push_back(MakeTransition(1, 10, 99, 0.1f, { { "Speed", AnimConditionOp::Less, 0.1f } }));
    CHECK(!instance.Build(missing, &error));

    AnimBlendTreeData nested = MakeLocomotion();
    nested.nodes[0].graph = 10;
    nested.links.erase(nested.links.begin());
    CHECK(!instance.Build(nested, &error));
    CHECK(error.find("nested") != std::string::npos);
}

TEST_CASE("anim pose: decompose undoes build and sockets follow their bone")
{
    const float t[3] = { 1.0f, 2.0f, 3.0f };
    const float r[4] = { 0.0f, 0.70710678f, 0.0f, 0.70710678f };
    const float s[3] = { 2.0f, 2.0f, 2.0f };
    float m[16];
    BuildTransformMatrix(t, r, s, m);
    float t2[3], r2[4], s2[3];
    DecomposeMatrix(m, t2, r2, s2);
    for (int i = 0; i < 3; ++i)
    {
        CHECK_NEAR(t2[i], t[i], 1e-5);
        CHECK_NEAR(s2[i], s[i], 1e-5);
    }
    const float dot = r[0] * r2[0] + r[1] * r2[1] + r[2] * r2[2] + r[3] * r2[3];
    CHECK_NEAR(std::fabs(dot), 1.0, 1e-5);

    KModelData model = MakeTwoNodeModel();
    KModelBone bone;
    bone.name = "arm";
    bone.nodeIndex = 1;
    model.bones.push_back(bone);
    std::vector<AnimationTransform> pose;
    std::vector<float> world;
    BuildAnimationBindPose(model, pose);
    BuildAnimationWorldMatrices(model, pose, world);
    const float offset[3] = { 0.5f, 0.0f, 0.0f };
    const float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float one[3] = { 1.0f, 1.0f, 1.0f };
    float socket[16];
    BuildBoneAttachmentMatrix(model, world, 0, offset, identity, one, socket);
    CHECK_NEAR(socket[12], 0.5, 1e-5);
    CHECK_NEAR(socket[13], 1.0, 1e-5);
}

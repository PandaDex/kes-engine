#pragma once
#include <string>
#include <vector>
#include "entity.h"
#include "animator.h"
#include "animation_clip.h"
#include "animation_graph.h"
#include "anim_blend_tree.h"
#include "kmodel.h"
#include "material.h"
#include "mesh.h"

// Properties beyond the model and animation paths:
//   AnimGraph a .kanimgraph blend tree, or the older state-machine graph
//   Collider  "Box" or "Sphere": a collider fitted to the mesh bounds. Without it the
//             colliders saved in the .kmodel are used, if it has any: those on the model
//             root form the entity's body, those on bones follow the animation as kinematic
//             bodies. A Dynamic model puts all of them in one rigid body in the bind pose.
//   Dynamic   true/false  simulate it as a rigid body (default false)
//   Density, Friction, Restitution  for dynamic models
class SkinnedModelEntity : public Entity
{
public:
    void Start() override;
    void Update(float dt) override;
    void Teleport(const Vector3& newPosition) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;
    bool OwnsCollider(ColliderHandle handle) const override;

    void SetModelPath(const std::string& path);
    void SetMaterialPath(const std::string& path);
    void SetAnimationClipPath(const std::string& path);
    void SetAnimationGraphPath(const std::string& path);
    void SetScale(const Vector3& value);
    const Vector3& GetScale() const { return scale; }
    const std::string& GetModelPath() const { return modelPath; }
    const std::string& GetMaterialPath() const { return materialPath; }
    const std::string& GetAnimationClipPath() const { return animationClipPath; }
    const std::string& GetAnimationGraphPath() const { return animationGraphPath; }
    const std::string& GetCurrentAnimationClipPath() const { return currentAnimationClipPath; }
    ModelHandle GetModelHandle() const { return modelHandle; }
    float GetCurrentAnimationTime() const { return currentAnimationTime; }
    int GetCurrentAnimationFrame() const { return currentAnimationFrame; }
    uint32_t GetCurrentBoneCount() const { return currentBoneCount; }
    const std::vector<float>& GetDebugBoneWorldMatrices() const { return debugBoneWorldMatrices; }

    // Graph parameters; also forwarded to a state-machine graph (a true bool fires a Trigger of that name).
    void SetAnimFloat(const std::string& name, float value);
    void SetAnimBool(const std::string& name, bool value);
    void SetAnimInt(const std::string& name, int value);
    // Socket transform in model space (before the entity transform) for the current pose.
    bool GetSocketModelMatrix(const std::string& name, float outMatrix[16]) const;

    // Applies to every child with that name, nested ones too. Returns false if there is none.
    // An empty animation path puts the child back in its bind pose.
    bool SetChildAnimation(const std::string& child, const std::string& path);
    bool SetChildAnimFloat(const std::string& child, const std::string& name, float value);
    bool SetChildAnimBool(const std::string& child, const std::string& name, bool value);
    bool SetChildAnimInt(const std::string& child, const std::string& name, int value);

private:
    void RefreshAssetHandles();
    const AnimationGraphData* ResolveAnimationGraph();
    bool ResolveBlendTree();
    void CreateModelColliders(const KModelData& model);
    void UpdateBoneColliders(const KModelData& model, const std::vector<AnimationTransform>& pose, float dt);
    const AnimationClipData* ResolveAnimationClipHandle(const std::string& path, AnimationClipHandle& handle, std::string& cachedPath);
    void SyncSceneObject(float dt);
    void CreateCollider();
    void CreateChildParts(ModelHandle parentModel, int parentPart, int depth);
    void DestroyChildParts();
    void UpdateChildParts(const KModelData& model, const std::vector<AnimationTransform>& pose, float dt);
    void OnDestroy() override;

    uint32_t objectId = 0;
    std::string colliderShape;
    ColliderHandle collider = 0;
    RigidBodyDesc body = MakeStaticDesc();

    static RigidBodyDesc MakeStaticDesc()
    {
        RigidBodyDesc desc;
        desc.motion = ColliderMotion::Static;
        return desc;
    }
    Vector3 scale = { 1.0f, 1.0f, 1.0f };
    std::string modelPath;
    std::string materialPath;
    std::string animationClipPath;
    std::string animationGraphPath;
    std::string cachedAnimationClipPath;
    std::string cachedPlaybackClipPath;
    std::string currentAnimationClipPath;
    MeshHandle meshHandle = 0;
    ModelHandle modelHandle = 0;
    MaterialHandle materialHandle = 0;
    AnimationClipHandle animationClipHandle = 0;
    AnimationGraphHandle animationGraphHandle = 0;
    AnimationClipHandle playbackClipHandle = 0;
    bool assetsDirty = true; // model or material path changed; each path is loaded once
    bool graphLoadAttempted = false;
    float clipTime = 0.0f;
    float currentAnimationTime = 0.0f;
    int currentAnimationFrame = 0;
    uint32_t currentBoneCount = 0;
    std::vector<AnimationTransform> localPoseScratch;
    std::vector<float> skinMatricesScratch;
    std::vector<float> debugBoneWorldMatrices;
    AnimatorGraphInstance animator = {};
    AnimBlendTreeInstance blendTree;
    bool blendTreeAttempted = false;
    std::vector<AnimationTransform> lastPose;

    struct BoneCollider
    {
        ColliderHandle handle = 0;
        size_t colliderIndex = 0;
    };
    std::vector<BoneCollider> boneColliders;
    std::vector<float> boneWorldScratch;

    // a parent is always stored before its children, UpdateChildParts relies on it
    struct ChildPart
    {
        uint32_t objectId = 0;
        ModelHandle model = 0;
        int parentPart = -1; // -1 = attached to the entity's own model
        size_t childIndex = 0;
        std::string name;
        std::vector<float> nodeWorld;
        float world[16] = {};

        std::string animationPath;
        AnimBlendTreeInstance blendTree;
        bool animationLoaded = false;
        AnimationClipHandle clip = 0;
        float clipTime = 0.0f;
        std::vector<AnimationTransform> pose;
        std::vector<float> skin;
    };
    void LoadChildAnimation(ChildPart& part);
    bool EvaluateChildAnimation(ChildPart& part, const KModelData& model, float dt);
    void ResetChildAnimation(ChildPart& part, const std::string& path);
    template<typename Fn>
    bool ForEachChildNamed(const std::string& name, Fn&& fn);
    std::vector<ChildPart> childParts;
    ModelHandle childPartsModel = 0;
    std::vector<float> rootWorldScratch;
};

#pragma once
#include <string>
#include <vector>
#include "entity.h"
#include "animator.h"
#include "animation_clip.h"
#include "animation_graph.h"
#include "kmodel.h"
#include "material.h"
#include "mesh.h"

class SkinnedModelEntity : public Entity
{
public:
    void Start() override;
    void Update(float dt) override;
    void Teleport(const Vector3& newPosition) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;

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

private:
    void RefreshAssetHandles();
    const AnimationGraphData* ResolveAnimationGraph();
    const AnimationClipData* ResolveAnimationClipHandle(const std::string& path, AnimationClipHandle& handle, std::string& cachedPath);
    void SyncSceneObject(float dt);
    void OnDestroy() override;

    uint32_t objectId = 0;
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
    float clipTime = 0.0f;
    float currentAnimationTime = 0.0f;
    int currentAnimationFrame = 0;
    uint32_t currentBoneCount = 0;
    std::vector<AnimationTransform> localPoseScratch;
    std::vector<float> skinMatricesScratch;
    std::vector<float> debugBoneWorldMatrices;
    AnimatorGraphInstance animator = {};
};

#include "ent_skinned_model.h"
#include <algorithm>
#include "animation_clip.h"
#include "animation_pose.h"
#include "asset_system.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include "scene.h"

void SkinnedModelEntity::Start()
{
    if (!game || !game->scene)
        return;

    SceneObject object = {};
    object.transform.position = Position;
    object.transform.rotation = Rotation;
    object.transform.scale = scale;

    if (game->assets)
    {
        RefreshAssetHandles();
        object.mesh = meshHandle;
        object.model = modelHandle;
        object.material = materialHandle;

        if (const AnimationGraphData* graph = ResolveAnimationGraph())
            InitializeAnimatorGraphInstance(*graph, animator);
    }

    objectId = game->scene->CreateObject(object);
    SyncSceneObject(0.0f);
}

void SkinnedModelEntity::Update(float dt)
{
    SyncSceneObject(dt);
}

void SkinnedModelEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    SyncSceneObject(0.0f);
}

bool SkinnedModelEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "Mesh" || name == "Model")
    {
        SetModelPath(entityprops::ParseString(value));
        return true;
    }

    if (name == "Material" || name == "MaterialFile")
    {
        SetMaterialPath(entityprops::ParseString(value));
        return true;
    }

    if (name == "AnimGraph")
    {
        SetAnimationGraphPath(entityprops::ParseString(value));
        return true;
    }

    if (name == "AnimClip" || name == "Animation")
    {
        SetAnimationClipPath(entityprops::ParseString(value));
        return true;
    }

    if (name == "Scale")
    {
        SetScale(entityprops::ParseVector3(value, scale));
        return true;
    }

    return Entity::ApplyProperty(name, value);
}

void SkinnedModelEntity::SetModelPath(const std::string& path)
{
    modelPath = path;
    meshHandle = 0;
    modelHandle = 0;
}

void SkinnedModelEntity::SetMaterialPath(const std::string& path)
{
    materialPath = path;
    materialHandle = 0;
}

void SkinnedModelEntity::SetAnimationGraphPath(const std::string& path)
{
    animationGraphPath = path;
    animator = {};
    animationGraphHandle = 0;
    playbackClipHandle = 0;
    cachedPlaybackClipPath.clear();
}

void SkinnedModelEntity::SetAnimationClipPath(const std::string& path)
{
    animationClipPath = path;
    animationClipHandle = 0;
    cachedAnimationClipPath.clear();
    clipTime = 0.0f;
}

void SkinnedModelEntity::SetScale(const Vector3& value)
{
    scale = value;
}

void SkinnedModelEntity::RefreshAssetHandles()
{
    if (!game || !game->assets)
        return;

    if (modelPath.empty())
    {
        meshHandle = 0;
        modelHandle = 0;
    }
    else
    {
        if (meshHandle == 0)
            meshHandle = game->assets->LoadMesh(modelPath.c_str());
        if (modelHandle == 0)
            modelHandle = game->assets->LoadModel(modelPath.c_str());
    }

    if (materialPath.empty())
    {
        materialHandle = 0;
    }
    else if (materialHandle == 0)
    {
        materialHandle = game->assets->LoadMaterial(materialPath.c_str());
    }
}

const AnimationGraphData* SkinnedModelEntity::ResolveAnimationGraph()
{
    if (!game || !game->assets || animationGraphPath.empty())
        return nullptr;

    if (animationGraphHandle == 0)
        animationGraphHandle = game->assets->LoadAnimationGraph(animationGraphPath.c_str());
    return game->assets->GetAnimationGraph(animationGraphHandle);
}

const AnimationClipData* SkinnedModelEntity::ResolveAnimationClipHandle(const std::string& path, AnimationClipHandle& handle, std::string& cachedPath)
{
    if (!game || !game->assets || path.empty())
        return nullptr;

    if (cachedPath != path)
    {
        cachedPath = path;
        handle = 0;
    }

    if (handle == 0)
        handle = game->assets->LoadAnimationClip(path.c_str());
    return game->assets->GetAnimationClip(handle);
}

void SkinnedModelEntity::SyncSceneObject(float dt)
{
    if (!game || !game->scene || !game->assets)
        return;
    if (objectId == 0)
        return;

    currentAnimationClipPath.clear();
    currentAnimationTime = 0.0f;
    currentAnimationFrame = 0;
    currentBoneCount = 0;
    debugBoneWorldMatrices.clear();

    SceneObject* object = game->scene->FindObject(objectId);
    if (!object)
        return;

    object->transform.position = Position;
    object->transform.rotation = Rotation;
    object->transform.scale = scale;
    RefreshAssetHandles();
    object->model = modelHandle;
    object->mesh = meshHandle;
    object->material = materialHandle;

    const KModelData* model = game->assets->GetModel(object->model);
    object->skinMatrixCount = 0;
    if (!model)
        return;

    if (model->bones.empty())
        return;

    currentBoneCount = (uint32_t)model->bones.size();

    std::vector<AnimationTransform>& localPose = localPoseScratch;
    localPose.clear();
    bool sampledClip = false;

    if (!animationGraphPath.empty())
    {
        const AnimationGraphData* graph = ResolveAnimationGraph();
        AnimatorPlaybackState playback = {};
        if (graph && UpdateAnimatorGraph(*graph, animator, dt, playback) && !playback.clipPath.empty())
        {
            const AnimationClipData* clip = ResolveAnimationClipHandle(playback.clipPath, playbackClipHandle, cachedPlaybackClipPath);
            if (clip)
            {
                SampleAnimationClip(*model, *clip, playback.clipTime, localPose);
                sampledClip = true;
                currentAnimationClipPath = playback.clipPath;
                currentAnimationTime = playback.clipTime;
            }
        }
    }

    if (!sampledClip && !animationClipPath.empty())
    {
        const AnimationClipData* clip = ResolveAnimationClipHandle(animationClipPath, animationClipHandle, cachedAnimationClipPath);
        if (clip)
        {
            clipTime += dt;
            SampleAnimationClip(*model, *clip, clipTime, localPose);
            sampledClip = true;
            currentAnimationClipPath = animationClipPath;
            currentAnimationTime = clipTime;
        }
    }

    if (localPose.empty())
        BuildAnimationBindPose(*model, localPose);

    if (game->animationDebugEnabled)
        BuildAnimationWorldMatrices(*model, localPose, debugBoneWorldMatrices);
    currentAnimationFrame = (int)(currentAnimationTime * 30.0f);

    if (!BuildAnimationSkinMatrices(*model, localPose, skinMatricesScratch))
        return;

    object->skinMatrixCount = (uint32_t)std::min<size_t>(model->bones.size(), SceneObject::MaxSkinMatrices);
    if (object->skinMatrixCount > 0)
    {
        const size_t copyCount = (size_t)object->skinMatrixCount * 16;
        for (size_t index = 0; index < copyCount; ++index)
            object->skinMatrices[index] = skinMatricesScratch[index];
    }
}

void SkinnedModelEntity::OnDestroy()
{
    if (game && game->scene)
        game->scene->RemoveObject(objectId);
}

#include "ent_skinned_model.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include "animation_clip.h"
#include "animation_pose.h"
#include "asset_system.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include "logger.h"
#include "scene.h"
#include "profiler.h"

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

        if (!IsAnimBlendTreePath(animationGraphPath))
        {
            if (const AnimationGraphData* graph = ResolveAnimationGraph())
                InitializeAnimatorGraphInstance(*graph, animator);
        }
    }

    objectId = game->scene->CreateObject(object);
    CreateCollider();
    SyncSceneObject(0.0f);
}

namespace
{
    // Collider placed by `matrix` (model space, from BuildBoneAttachmentMatrix with unit scale),
    // then scaled by the entity. The shape's centre and rotation are its pose in entity space.
    RigidShape MakeModelShape(const KModelCollider& source, const float matrix[16], const Vector3& scale)
    {
        float position[3], rotation[4], boneScale[3];
        DecomposeMatrix(matrix, position, rotation, boneScale);
        const float boneSize = (boneScale[0] + boneScale[1] + boneScale[2]) / 3.0f;
        const float sizeScale = std::max({ std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z) });

        RigidShape shape;
        shape.type = source.shape == KModelColliderShape::Sphere    ? ColliderType::Sphere
                     : source.shape == KModelColliderShape::Capsule ? ColliderType::Capsule
                                                                    : ColliderType::Box;
        shape.center = { position[0] * scale.x, position[1] * scale.y, position[2] * scale.z };
        std::copy(rotation, rotation + 4, shape.rotation);
        shape.halfExtents = { source.halfExtents[0] * boneScale[0] * std::fabs(scale.x),
                              source.halfExtents[1] * boneScale[1] * std::fabs(scale.y),
                              source.halfExtents[2] * boneScale[2] * std::fabs(scale.z) };
        shape.radius = source.radius * boneSize * sizeScale;
        shape.halfHeight = source.halfHeight * boneSize * sizeScale;
        return shape;
    }
}

bool SkinnedModelEntity::OwnsCollider(ColliderHandle handle) const
{
    if (handle == 0)
        return false;
    if (handle == collider)
        return true;
    return std::any_of(boneColliders.begin(), boneColliders.end(), [handle](const BoneCollider& b) { return b.handle == handle; });
}

void SkinnedModelEntity::CreateModelColliders(const KModelData& model)
{
    std::vector<AnimationTransform> bindPose;
    std::vector<float> worldMatrices;
    BuildAnimationBindPose(model, bindPose);
    BuildAnimationWorldMatrices(model, bindPose, worldMatrices);

    const bool followBones = body.motion != ColliderMotion::Dynamic && !model.bones.empty();
    const float unitScale[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<RigidShape> rootShapes;
    for (size_t index = 0; index < model.colliders.size(); ++index)
    {
        const KModelCollider& source = model.colliders[index];
        float matrix[16];
        BuildBoneAttachmentMatrix(model, worldMatrices, source.boneIndex, source.center, source.rotation, unitScale, matrix);
        RigidShape shape = MakeModelShape(source, matrix, scale);
        if (!followBones || source.boneIndex < 0)
        {
            rootShapes.push_back(shape);
            continue;
        }

        // Its own kinematic body, with the shape at the body origin; the body carries the pose.
        const Vector3 localPosition = shape.center;
        float localRotation[4];
        std::copy(shape.rotation, shape.rotation + 4, localRotation);
        shape.center = {};
        shape.rotation[0] = shape.rotation[1] = shape.rotation[2] = 0.0f;
        shape.rotation[3] = 1.0f;

        RigidBodyDesc desc = body;
        desc.motion = ColliderMotion::Kinematic;
        desc.position = Position;
        desc.rotation = Rotation;
        const ColliderHandle handle = game->collision->AddRigidShapes({ shape }, desc);
        if (handle == 0)
            continue;
        game->collision->SetTargetPose(handle, Position, Rotation, localPosition, localRotation, 0.0f);
        boneColliders.push_back({ handle, index });
    }

    if (!rootShapes.empty())
    {
        body.position = Position;
        body.rotation = Rotation;
        collider = game->collision->AddRigidShapes(rootShapes, body);
    }
}

void SkinnedModelEntity::UpdateBoneColliders(const KModelData& model, const std::vector<AnimationTransform>& pose, float dt)
{
    if (boneColliders.empty() || !game || !game->collision)
        return;
    BuildAnimationWorldMatrices(model, pose, boneWorldScratch);
    const float unitScale[3] = { 1.0f, 1.0f, 1.0f };
    for (const BoneCollider& bone : boneColliders)
    {
        if (bone.colliderIndex >= model.colliders.size())
            continue;
        const KModelCollider& source = model.colliders[bone.colliderIndex];
        float matrix[16];
        BuildBoneAttachmentMatrix(model, boneWorldScratch, source.boneIndex, source.center, source.rotation, unitScale, matrix);
        const RigidShape placed = MakeModelShape(source, matrix, scale);
        game->collision->SetTargetPose(bone.handle, Position, Rotation, placed.center, placed.rotation, dt);
    }
}

void SkinnedModelEntity::CreateCollider()
{
    if (!game->collision)
        return;
    if (colliderShape.empty())
    {
        const KModelData* model = game->assets ? game->assets->GetModel(modelHandle) : nullptr;
        if (model && !model->colliders.empty())
            CreateModelColliders(*model);
        return;
    }
    const MeshData* mesh = game->assets ? game->assets->GetMesh(meshHandle) : nullptr;
    if (!mesh || mesh->vertices.empty())
    {
        EngineLogf("Model %s has no mesh to fit a collider to.", modelPath.c_str());
        return;
    }

    Vector3 minV = { FLT_MAX, FLT_MAX, FLT_MAX };
    Vector3 maxV = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const MeshVertex& vertex : mesh->vertices)
    {
        minV = { std::min(minV.x, vertex.px), std::min(minV.y, vertex.py), std::min(minV.z, vertex.pz) };
        maxV = { std::max(maxV.x, vertex.px), std::max(maxV.y, vertex.py), std::max(maxV.z, vertex.pz) };
    }
    const Vector3 center = (minV + maxV) * 0.5f;
    const Vector3 offset = { center.x * scale.x, center.y * scale.y, center.z * scale.z };
    const Vector3 half = { (maxV.x - minV.x) * 0.5f * std::fabs(scale.x), (maxV.y - minV.y) * 0.5f * std::fabs(scale.y),
                           (maxV.z - minV.z) * 0.5f * std::fabs(scale.z) };

    body.position = Position;
    body.rotation = Rotation;
    if (colliderShape == "Sphere")
        collider = game->collision->AddRigidSphere(std::max({ half.x, half.y, half.z }), offset, body);
    else if (colliderShape == "Box")
        collider = game->collision->AddRigidBox(half, offset, body);
    else
        EngineLogf("Unknown collider \"%s\" on %s; use Box or Sphere.", colliderShape.c_str(), modelPath.c_str());
}

void SkinnedModelEntity::Update(float dt)
{
    if (collider != 0 && body.motion == ColliderMotion::Dynamic && game && game->collision)
        game->collision->GetBodyPose(collider, game->fixedStepAlpha, Position, Rotation);
    SyncSceneObject(dt);
}

void SkinnedModelEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    if (collider != 0 && game && game->collision)
        game->collision->SetPosition(collider, newPosition);
    SyncSceneObject(0.0f);   // a zero step teleports the bone colliders
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

    if (name == "Collider")
    {
        colliderShape = entityprops::ParseString(value);
        return true;
    }

    if (ApplyRigidBodyProperty(body, name, value))
        return true;

    return Entity::ApplyProperty(name, value);
}

void SkinnedModelEntity::SetModelPath(const std::string& path)
{
    modelPath = path;
    meshHandle = 0;
    modelHandle = 0;
    assetsDirty = true;
}

void SkinnedModelEntity::SetMaterialPath(const std::string& path)
{
    materialPath = path;
    materialHandle = 0;
    assetsDirty = true;
}

void SkinnedModelEntity::SetAnimationGraphPath(const std::string& path)
{
    animationGraphPath = path;
    blendTree = {};
    blendTreeAttempted = false;
    animator = {};
    animationGraphHandle = 0;
    graphLoadAttempted = false;
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
    if (!assetsDirty || !game || !game->assets)
        return;
    assetsDirty = false;

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
        const KModelData* model = game->assets->GetModel(modelHandle);
        if (model && model->bones.size() > SceneObject::MaxSkinMatrices)
            EngineLogf("Model %s has %zu bones; only the first %u are animated.", modelPath.c_str(), model->bones.size(),
                       SceneObject::MaxSkinMatrices);
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

    if (!graphLoadAttempted)
    {
        graphLoadAttempted = true;
        animationGraphHandle = game->assets->LoadAnimationGraph(animationGraphPath.c_str());
    }
    return game->assets->GetAnimationGraph(animationGraphHandle);
}

bool SkinnedModelEntity::ResolveBlendTree()
{
    if (!game || !game->assets || !IsAnimBlendTreePath(animationGraphPath))
        return false;
    if (!blendTreeAttempted)
    {
        blendTreeAttempted = true;
        const AnimBlendTreeData* data = game->assets->GetAnimBlendTree(game->assets->LoadAnimBlendTree(animationGraphPath.c_str()));
        std::string error;
        if (data && !blendTree.Build(*data, &error))
            EngineLogf("Animation graph %s: %s", animationGraphPath.c_str(), error.c_str());
    }
    return blendTree.IsBuilt();
}

void SkinnedModelEntity::SetAnimFloat(const std::string& name, float value)
{
    if (ResolveBlendTree())
        blendTree.SetFloat(name, value);
    else if (const AnimationGraphData* graph = ResolveAnimationGraph())
        SetAnimatorFloat(*graph, animator, name.c_str(), value);
}

void SkinnedModelEntity::SetAnimBool(const std::string& name, bool value)
{
    if (ResolveBlendTree())
        blendTree.SetBool(name, value);
    else if (const AnimationGraphData* graph = ResolveAnimationGraph())
    {
        SetAnimatorBool(*graph, animator, name.c_str(), value);
        if (value)
            FireAnimatorTrigger(*graph, animator, name.c_str());
    }
}

void SkinnedModelEntity::SetAnimInt(const std::string& name, int value)
{
    if (ResolveBlendTree())
        blendTree.SetInt(name, value);
    else
        SetAnimFloat(name, (float)value);
}

bool SkinnedModelEntity::GetSocketModelMatrix(const std::string& name, float outMatrix[16]) const
{
    const KModelData* model = game && game->assets ? game->assets->GetModel(modelHandle) : nullptr;
    const KModelSocket* socket = model ? FindKModelSocket(*model, name.c_str()) : nullptr;
    if (!socket)
        return false;

    std::vector<AnimationTransform> pose = lastPose;
    if (pose.size() != model->nodes.size())
        BuildAnimationBindPose(*model, pose);
    std::vector<float> worldMatrices;
    BuildAnimationWorldMatrices(*model, pose, worldMatrices);
    BuildBoneAttachmentMatrix(*model, worldMatrices, socket->boneIndex, socket->translation, socket->rotation, socket->scale, outMatrix);
    return true;
}

const AnimationClipData* SkinnedModelEntity::ResolveAnimationClipHandle(const std::string& path, AnimationClipHandle& handle, std::string& cachedPath)
{
    if (!game || !game->assets || path.empty())
        return nullptr;

    if (cachedPath != path)
    {
        cachedPath = path;
        handle = game->assets->LoadAnimationClip(path.c_str());
    }
    return game->assets->GetAnimationClip(handle);
}

void SkinnedModelEntity::SyncSceneObject(float dt)
{
    KES_ZONE_FN(WORLD);
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

    if (ResolveBlendTree())
    {
        const AnimBlendTreeInstance::ClipResolver clips = [this](const std::string& path)
        { return game->assets->GetAnimationClip(game->assets->LoadAnimationClip(path.c_str())); };
        if (blendTree.Evaluate(*model, dt, clips, localPose))
        {
            sampledClip = true;
            currentAnimationClipPath = animationGraphPath;
        }
    }
    else if (!animationGraphPath.empty())
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
    lastPose = localPose;
    UpdateBoneColliders(*model, localPose, dt);

    if (game->animationDebugEnabled)
        BuildAnimationWorldMatrices(*model, localPose, debugBoneWorldMatrices);
    currentAnimationFrame = (int)(currentAnimationTime * 30.0f);

    if (!BuildAnimationSkinMatrices(*model, localPose, skinMatricesScratch))
        return;

    object->skinMatrixCount = (uint32_t)std::min<size_t>(model->bones.size(), SceneObject::MaxSkinMatrices);
    object->skinMatrices.assign(skinMatricesScratch.begin(), skinMatricesScratch.begin() + (size_t)object->skinMatrixCount * 16);
}

void SkinnedModelEntity::OnDestroy()
{
    if (game && game->scene)
        game->scene->RemoveObject(objectId);
    if (game && game->collision && collider != 0)
        game->collision->Remove(collider);
    collider = 0;
    for (const BoneCollider& bone : boneColliders)
    {
        if (game && game->collision)
            game->collision->Remove(bone.handle);
    }
    boneColliders.clear();
}

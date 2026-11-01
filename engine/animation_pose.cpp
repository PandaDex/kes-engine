#include "animation_pose.h"
#include <cstring>
#include "kmodel.h"

namespace
{
    struct Float3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct Matrix4
    {
        float m[16] = {};
    };

    static Matrix4 Identity()
    {
        Matrix4 matrix = {};
        matrix.m[0] = 1.0f;
        matrix.m[5] = 1.0f;
        matrix.m[10] = 1.0f;
        matrix.m[15] = 1.0f;
        return matrix;
    }

    static Matrix4 Multiply(const Matrix4& a, const Matrix4& b)
    {
        Matrix4 result = {};
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                result.m[row * 4 + col] =
                    a.m[row * 4 + 0] * b.m[0 * 4 + col] +
                    a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                    a.m[row * 4 + 2] * b.m[2 * 4 + col] +
                    a.m[row * 4 + 3] * b.m[3 * 4 + col];
            }
        }
        return result;
    }

    static Float3 RotateVectorByQuaternion(const Float3& value, const float rotation[4])
    {
        const Float3 qv = { rotation[0], rotation[1], rotation[2] };
        const Float3 uv = {
            qv.y * value.z - qv.z * value.y,
            qv.z * value.x - qv.x * value.z,
            qv.x * value.y - qv.y * value.x
        };
        const Float3 uuv = {
            qv.y * uv.z - qv.z * uv.y,
            qv.z * uv.x - qv.x * uv.z,
            qv.x * uv.y - qv.y * uv.x
        };

        return {
            value.x + ((uv.x * rotation[3]) + uuv.x) * 2.0f,
            value.y + ((uv.y * rotation[3]) + uuv.y) * 2.0f,
            value.z + ((uv.z * rotation[3]) + uuv.z) * 2.0f
        };
    }

    static Matrix4 BuildLocalMatrix(const AnimationTransform& transform)
    {
        Matrix4 matrix = Identity();

        const Float3 xAxis = RotateVectorByQuaternion({ transform.scale[0], 0.0f, 0.0f }, transform.rotation);
        const Float3 yAxis = RotateVectorByQuaternion({ 0.0f, transform.scale[1], 0.0f }, transform.rotation);
        const Float3 zAxis = RotateVectorByQuaternion({ 0.0f, 0.0f, transform.scale[2] }, transform.rotation);

        matrix.m[0] = xAxis.x;
        matrix.m[1] = xAxis.y;
        matrix.m[2] = xAxis.z;

        matrix.m[4] = yAxis.x;
        matrix.m[5] = yAxis.y;
        matrix.m[6] = yAxis.z;

        matrix.m[8] = zAxis.x;
        matrix.m[9] = zAxis.y;
        matrix.m[10] = zAxis.z;

        matrix.m[12] = transform.translation[0];
        matrix.m[13] = transform.translation[1];
        matrix.m[14] = transform.translation[2];
        return matrix;
    }

}

void BuildAnimationWorldMatrices(const KModelData& model, const std::vector<AnimationTransform>& localPose, std::vector<float>& outWorldMatrices)
{
    const size_t nodeCount = model.nodes.size();
    outWorldMatrices.resize(nodeCount * 16);
    if (nodeCount == 0)
        return;

    // Scratch reused across calls (this runs per skinned model per frame).
    thread_local std::vector<Matrix4> localMatrices;
    thread_local std::vector<Matrix4> worldMatrices;
    localMatrices.assign(nodeCount, Identity());
    worldMatrices.resize(nodeCount);

    for (size_t nodeIndex = 0; nodeIndex < nodeCount && nodeIndex < localPose.size(); ++nodeIndex)
        localMatrices[nodeIndex] = BuildLocalMatrix(localPose[nodeIndex]);

    for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
    {
        const int32_t parentIndex = model.nodes[nodeIndex].parentIndex;
        Matrix4 world;
        if (parentIndex < 0 || parentIndex >= static_cast<int32_t>(nodeCount))
        {
            world = localMatrices[nodeIndex];
        }
        else if (static_cast<size_t>(parentIndex) < nodeIndex)
        {
            // Parent already resolved (the usual parent-before-child order): O(1).
            world = Multiply(localMatrices[nodeIndex], worldMatrices[static_cast<size_t>(parentIndex)]);
        }
        else
        {
            // Out-of-order hierarchy: walk the chain.
            world = localMatrices[nodeIndex];
            int32_t ancestor = parentIndex;
            size_t guard = 0;
            while (ancestor >= 0 && ancestor < static_cast<int32_t>(nodeCount) && guard++ < nodeCount)
            {
                world = Multiply(world, localMatrices[static_cast<size_t>(ancestor)]);
                ancestor = model.nodes[static_cast<size_t>(ancestor)].parentIndex;
            }
        }
        worldMatrices[nodeIndex] = world;
        std::memcpy(outWorldMatrices.data() + nodeIndex * 16, world.m, sizeof(world.m));
    }
}

bool BuildAnimationSkinMatrices(const KModelData& model, const std::vector<AnimationTransform>& localPose, std::vector<float>& outSkinMatrices, std::string* outError)
{
    if (model.bones.empty())
    {
        outSkinMatrices.clear();
        return true;
    }

    thread_local std::vector<float> worldMatrices;
    BuildAnimationWorldMatrices(model, localPose, worldMatrices);

    outSkinMatrices.resize(model.bones.size() * 16);
    for (size_t boneIndex = 0; boneIndex < model.bones.size(); ++boneIndex)
    {
        const KModelBone& bone = model.bones[boneIndex];
        if (bone.nodeIndex < 0 || bone.nodeIndex >= (int32_t)model.nodes.size())
        {
            if (outError)
                *outError = "Animation bone references invalid node index";
            return false;
        }

        Matrix4 inverseBind = {};
        std::memcpy(inverseBind.m, bone.inverseBindMatrix, sizeof(inverseBind.m));
        Matrix4 world = {};
        std::memcpy(world.m, worldMatrices.data() + (size_t)bone.nodeIndex * 16, sizeof(world.m));
        const Matrix4 skin = Multiply(inverseBind, world);
        std::memcpy(outSkinMatrices.data() + boneIndex * 16, skin.m, sizeof(skin.m));
    }

    return true;
}

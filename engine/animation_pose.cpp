#include "animation_pose.h"
#include <cmath>
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

void BuildTransformMatrix(const float translation[3], const float rotation[4], const float scale[3], float outMatrix[16])
{
    AnimationTransform transform;
    std::memcpy(transform.translation, translation, sizeof(transform.translation));
    std::memcpy(transform.rotation, rotation, sizeof(transform.rotation));
    std::memcpy(transform.scale, scale, sizeof(transform.scale));
    const Matrix4 matrix = BuildLocalMatrix(transform);
    std::memcpy(outMatrix, matrix.m, sizeof(matrix.m));
}

void MultiplyMatrices(const float first[16], const float then[16], float outMatrix[16])
{
    Matrix4 a, b;
    std::memcpy(a.m, first, sizeof(a.m));
    std::memcpy(b.m, then, sizeof(b.m));
    const Matrix4 result = Multiply(a, b);
    std::memcpy(outMatrix, result.m, sizeof(result.m));
}

void DecomposeMatrix(const float m[16], float outTranslation[3], float outRotation[4], float outScale[3])
{
    for (int i = 0; i < 3; ++i)
        outTranslation[i] = m[12 + i];

    float axes[3][3];
    for (int row = 0; row < 3; ++row)
    {
        const float length = std::sqrt(m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2]);
        outScale[row] = length;
        for (int col = 0; col < 3; ++col)
            axes[row][col] = length > 1e-8f ? m[row * 4 + col] / length : (row == col ? 1.0f : 0.0f);
    }

    // Rows hold the rotated axes, so R[i][j] = axes[j][i].
    const float r00 = axes[0][0], r11 = axes[1][1], r22 = axes[2][2];
    const float r21 = axes[1][2], r12 = axes[2][1];
    const float r02 = axes[2][0], r20 = axes[0][2];
    const float r10 = axes[0][1], r01 = axes[1][0];
    float x, y, z, w;
    const float trace = r00 + r11 + r22;
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        w = 0.25f * s;
        x = (r21 - r12) / s;
        y = (r02 - r20) / s;
        z = (r10 - r01) / s;
    }
    else if (r00 > r11 && r00 > r22)
    {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        w = (r21 - r12) / s;
        x = 0.25f * s;
        y = (r01 + r10) / s;
        z = (r02 + r20) / s;
    }
    else if (r11 > r22)
    {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        w = (r02 - r20) / s;
        x = (r01 + r10) / s;
        y = 0.25f * s;
        z = (r12 + r21) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        w = (r10 - r01) / s;
        x = (r02 + r20) / s;
        y = (r12 + r21) / s;
        z = 0.25f * s;
    }
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    const float inv = length > 1e-8f ? 1.0f / length : 0.0f;
    outRotation[0] = x * inv;
    outRotation[1] = y * inv;
    outRotation[2] = z * inv;
    outRotation[3] = length > 1e-8f ? w * inv : 1.0f;
}

void BuildBoneAttachmentMatrix(const KModelData& model, const std::vector<float>& nodeWorldMatrices, int32_t boneIndex,
                               const float translation[3], const float rotation[4], const float scale[3], float outMatrix[16])
{
    float local[16];
    BuildTransformMatrix(translation, rotation, scale, local);
    const int32_t node = boneIndex >= 0 && boneIndex < (int32_t)model.bones.size() ? model.bones[(size_t)boneIndex].nodeIndex : -1;
    if (node < 0 || (size_t)(node + 1) * 16 > nodeWorldMatrices.size())
    {
        std::memcpy(outMatrix, local, sizeof(local));
        return;
    }
    MultiplyMatrices(local, nodeWorldMatrices.data() + (size_t)node * 16, outMatrix);
}

#include "gltf_importer.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include "animation_clip.h"
#include "kmodel.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace
{
    struct Float3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    static Float3 Normalize(const Float3& value)
    {
        const float lengthSq = value.x * value.x + value.y * value.y + value.z * value.z;
        if (lengthSq <= 0.000001f)
            return { 0.0f, 1.0f, 0.0f };
        const float invLength = 1.0f / std::sqrt(lengthSq);
        return { value.x * invLength, value.y * invLength, value.z * invLength };
    }

    static int32_t PointerIndex(const cgltf_node* base, cgltf_size count, const cgltf_node* value)
    {
        if (!base || !value)
            return -1;
        const ptrdiff_t index = value - base;
        return index >= 0 && index < (ptrdiff_t)count ? (int32_t)index : -1;
    }

    static int32_t PointerIndex(const cgltf_mesh* base, cgltf_size count, const cgltf_mesh* value)
    {
        if (!base || !value)
            return -1;
        const ptrdiff_t index = value - base;
        return index >= 0 && index < (ptrdiff_t)count ? (int32_t)index : -1;
    }

    static void MatrixToQuaternion(const float matrix[16], float outRotation[4])
    {
        const float m00 = matrix[0];
        const float m11 = matrix[5];
        const float m22 = matrix[10];
        const float trace = m00 + m11 + m22;

        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            outRotation[3] = 0.25f * s;
            outRotation[0] = (matrix[6] - matrix[9]) / s;
            outRotation[1] = (matrix[8] - matrix[2]) / s;
            outRotation[2] = (matrix[1] - matrix[4]) / s;
            return;
        }

        if (m00 > m11 && m00 > m22)
        {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            outRotation[3] = (matrix[6] - matrix[9]) / s;
            outRotation[0] = 0.25f * s;
            outRotation[1] = (matrix[4] + matrix[1]) / s;
            outRotation[2] = (matrix[8] + matrix[2]) / s;
            return;
        }

        if (m11 > m22)
        {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            outRotation[3] = (matrix[8] - matrix[2]) / s;
            outRotation[0] = (matrix[4] + matrix[1]) / s;
            outRotation[1] = 0.25f * s;
            outRotation[2] = (matrix[9] + matrix[6]) / s;
            return;
        }

        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        outRotation[3] = (matrix[1] - matrix[4]) / s;
        outRotation[0] = (matrix[8] + matrix[2]) / s;
        outRotation[1] = (matrix[9] + matrix[6]) / s;
        outRotation[2] = 0.25f * s;
    }

    static void ExtractNodeTransform(const cgltf_node& node, KModelNode& outNode)
    {
        if (!node.has_matrix)
        {
            if (node.has_translation)
                std::memcpy(outNode.translation, node.translation, sizeof(outNode.translation));
            if (node.has_rotation)
                std::memcpy(outNode.rotation, node.rotation, sizeof(outNode.rotation));
            if (node.has_scale)
                std::memcpy(outNode.scale, node.scale, sizeof(outNode.scale));
            return;
        }

        outNode.translation[0] = node.matrix[12];
        outNode.translation[1] = node.matrix[13];
        outNode.translation[2] = node.matrix[14];

        const Float3 basisX = { node.matrix[0], node.matrix[1], node.matrix[2] };
        const Float3 basisY = { node.matrix[4], node.matrix[5], node.matrix[6] };
        const Float3 basisZ = { node.matrix[8], node.matrix[9], node.matrix[10] };

        outNode.scale[0] = std::sqrt(basisX.x * basisX.x + basisX.y * basisX.y + basisX.z * basisX.z);
        outNode.scale[1] = std::sqrt(basisY.x * basisY.x + basisY.y * basisY.y + basisY.z * basisY.z);
        outNode.scale[2] = std::sqrt(basisZ.x * basisZ.x + basisZ.y * basisZ.y + basisZ.z * basisZ.z);

        float rotationMatrix[16] = {
            basisX.x / (outNode.scale[0] > 0.000001f ? outNode.scale[0] : 1.0f),
            basisX.y / (outNode.scale[0] > 0.000001f ? outNode.scale[0] : 1.0f),
            basisX.z / (outNode.scale[0] > 0.000001f ? outNode.scale[0] : 1.0f),
            0.0f,
            basisY.x / (outNode.scale[1] > 0.000001f ? outNode.scale[1] : 1.0f),
            basisY.y / (outNode.scale[1] > 0.000001f ? outNode.scale[1] : 1.0f),
            basisY.z / (outNode.scale[1] > 0.000001f ? outNode.scale[1] : 1.0f),
            0.0f,
            basisZ.x / (outNode.scale[2] > 0.000001f ? outNode.scale[2] : 1.0f),
            basisZ.y / (outNode.scale[2] > 0.000001f ? outNode.scale[2] : 1.0f),
            basisZ.z / (outNode.scale[2] > 0.000001f ? outNode.scale[2] : 1.0f),
            0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        MatrixToQuaternion(rotationMatrix, outNode.rotation);
    }

    static const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type, int index = 0)
    {
        for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
        {
            const cgltf_attribute& attribute = primitive.attributes[i];
            if (attribute.type == type && attribute.index == index)
                return attribute.data;
        }
        return nullptr;
    }

    static bool ReadFloatAccessor(const cgltf_accessor* accessor, cgltf_size index, float* outValues, cgltf_size count)
    {
        if (!accessor)
            return false;
        return cgltf_accessor_read_float(accessor, index, outValues, count);
    }

    static bool ReadUintAccessor(const cgltf_accessor* accessor, cgltf_size index, uint32_t* outValues, cgltf_size count)
    {
        if (!accessor)
            return false;
        return cgltf_accessor_read_uint(accessor, index, outValues, count);
    }

    static std::string SanitizeName(const std::string& value, const char* fallback, size_t suffix)
    {
        std::string result;
        for (char ch : value)
        {
            const bool alphaNum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
            result.push_back(alphaNum ? ch : '_');
        }

        while (!result.empty() && result.back() == '_')
            result.pop_back();

        if (result.empty())
            result = fallback ? fallback : "clip";
        if (suffix > 0)
            result += "_" + std::to_string(suffix);
        return result;
    }

    static bool ImportPrimitive(const cgltf_primitive& primitive, const cgltf_skin* skin, const KModelData& model, const cgltf_data& data, KModelMesh& outMesh, std::string& error)
    {
        if (primitive.type != cgltf_primitive_type_triangles)
        {
            error = "Only triangle primitives are supported";
            return false;
        }

        const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position);
        if (!positions)
        {
            error = "Primitive is missing POSITION";
            return false;
        }

        const cgltf_accessor* normals = FindAttribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* tangents = FindAttribute(primitive, cgltf_attribute_type_tangent);
        const cgltf_accessor* texcoords = FindAttribute(primitive, cgltf_attribute_type_texcoord, 0);
        const cgltf_accessor* colors = FindAttribute(primitive, cgltf_attribute_type_color, 0);
        const cgltf_accessor* joints = FindAttribute(primitive, cgltf_attribute_type_joints, 0);
        const cgltf_accessor* weights = FindAttribute(primitive, cgltf_attribute_type_weights, 0);

        const uint32_t baseVertex = (uint32_t)outMesh.vertices.size();
        const cgltf_size vertexCount = positions->count;
        outMesh.vertices.reserve(outMesh.vertices.size() + vertexCount);

        for (cgltf_size vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            KModelVertex vertex = {};

            float values[4] = {};
            ReadFloatAccessor(positions, vertexIndex, values, 3);
            vertex.px = values[0];
            vertex.py = values[1];
            vertex.pz = values[2];

            if (ReadFloatAccessor(normals, vertexIndex, values, 3))
            {
                const Float3 normal = Normalize({ values[0], values[1], values[2] });
                vertex.nx = normal.x;
                vertex.ny = normal.y;
                vertex.nz = normal.z;
            }

            if (ReadFloatAccessor(tangents, vertexIndex, values, 4))
            {
                vertex.tx = values[0];
                vertex.ty = values[1];
                vertex.tz = values[2];
                vertex.tw = values[3];
            }

            if (ReadFloatAccessor(texcoords, vertexIndex, values, 2))
            {
                vertex.u = values[0];
                vertex.v = values[1];
            }

            vertex.r = 1.0f;
            vertex.g = 1.0f;
            vertex.b = 1.0f;
            vertex.a = 1.0f;
            if (colors)
            {
                const cgltf_size components = cgltf_num_components(colors->type);
                if (ReadFloatAccessor(colors, vertexIndex, values, components))
                {
                    if (components > 0) vertex.r = values[0];
                    if (components > 1) vertex.g = values[1];
                    if (components > 2) vertex.b = values[2];
                    if (components > 3) vertex.a = values[3];
                }
            }

            uint32_t jointValues[4] = { 0, 0, 0, 0 };
            if (ReadUintAccessor(joints, vertexIndex, jointValues, 4))
            {
                for (int i = 0; i < 4; ++i)
                {
                    uint16_t boneIndex = (uint16_t)jointValues[i];
                    if (skin && jointValues[i] < skin->joints_count)
                    {
                        const int32_t nodeIndex = PointerIndex(data.nodes, data.nodes_count, skin->joints[jointValues[i]]);
                        if (nodeIndex >= 0 && nodeIndex < (int32_t)model.nodes.size() && model.nodes[nodeIndex].boneIndex >= 0)
                            boneIndex = (uint16_t)model.nodes[nodeIndex].boneIndex;
                    }
                    vertex.boneIndices[i] = boneIndex;
                }
            }

            if (ReadFloatAccessor(weights, vertexIndex, values, 4))
            {
                float weightSum = 0.0f;
                for (int i = 0; i < 4; ++i)
                {
                    vertex.boneWeights[i] = values[i];
                    weightSum += values[i];
                }

                if (weightSum > 0.00001f)
                {
                    const float invWeightSum = 1.0f / weightSum;
                    for (float& weight : vertex.boneWeights)
                        weight *= invWeightSum;
                }
                else
                {
                    vertex.boneWeights[0] = 1.0f;
                }
            }

            outMesh.vertices.push_back(vertex);
        }

        KModelPrimitive outPrimitive = {};
        outPrimitive.firstIndex = (uint32_t)outMesh.indices.size();
        outPrimitive.materialIndex = -1;

        if (primitive.indices)
        {
            outPrimitive.indexCount = (uint32_t)primitive.indices->count;
            outMesh.indices.reserve(outMesh.indices.size() + primitive.indices->count);
            for (cgltf_size index = 0; index < primitive.indices->count; ++index)
                outMesh.indices.push_back(baseVertex + (uint32_t)cgltf_accessor_read_index(primitive.indices, index));
        }
        else
        {
            outPrimitive.indexCount = (uint32_t)vertexCount;
            outMesh.indices.reserve(outMesh.indices.size() + vertexCount);
            for (cgltf_size index = 0; index < vertexCount; ++index)
                outMesh.indices.push_back(baseVertex + (uint32_t)index);
        }

        outMesh.primitives.push_back(outPrimitive);
        return true;
    }

    static const cgltf_skin* FindMeshSkin(const cgltf_data& data, const cgltf_mesh* mesh)
    {
        if (!mesh)
            return nullptr;

        for (cgltf_size nodeIndex = 0; nodeIndex < data.nodes_count; ++nodeIndex)
        {
            const cgltf_node& node = data.nodes[nodeIndex];
            if (node.mesh == mesh && node.skin)
                return node.skin;
        }

        return nullptr;
    }

    static bool ImportMeshes(const cgltf_data& data, KModelData& model, std::string& error)
    {
        model.meshes.resize(data.meshes_count);
        for (cgltf_size meshIndex = 0; meshIndex < data.meshes_count; ++meshIndex)
        {
            const cgltf_mesh& sourceMesh = data.meshes[meshIndex];
            KModelMesh& mesh = model.meshes[meshIndex];
            mesh.name = sourceMesh.name ? sourceMesh.name : ("Mesh" + std::to_string(meshIndex));
            const cgltf_skin* skin = FindMeshSkin(data, &sourceMesh);

            for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex)
            {
                if (!ImportPrimitive(sourceMesh.primitives[primitiveIndex], skin, model, data, mesh, error))
                {
                    error = "Mesh " + mesh.name + ": " + error;
                    return false;
                }
            }
        }
        return true;
    }

    static void ImportNodes(const cgltf_data& data, KModelData& model)
    {
        model.nodes.resize(data.nodes_count);
        for (cgltf_size nodeIndex = 0; nodeIndex < data.nodes_count; ++nodeIndex)
        {
            const cgltf_node& source = data.nodes[nodeIndex];
            KModelNode& node = model.nodes[nodeIndex];
            node.name = source.name ? source.name : ("Node" + std::to_string(nodeIndex));
            node.parentIndex = PointerIndex(data.nodes, data.nodes_count, source.parent);
            node.meshIndex = PointerIndex(data.meshes, data.meshes_count, source.mesh);
            node.boneIndex = -1;
            ExtractNodeTransform(source, node);
        }
    }

    static void ImportBones(const cgltf_data& data, KModelData& model)
    {
        std::unordered_map<int32_t, int32_t> boneByNodeIndex;

        for (cgltf_size skinIndex = 0; skinIndex < data.skins_count; ++skinIndex)
        {
            const cgltf_skin& skin = data.skins[skinIndex];
            for (cgltf_size jointIndex = 0; jointIndex < skin.joints_count; ++jointIndex)
            {
                const int32_t nodeIndex = PointerIndex(data.nodes, data.nodes_count, skin.joints[jointIndex]);
                if (nodeIndex < 0)
                    continue;

                int32_t boneIndex = -1;
                auto existing = boneByNodeIndex.find(nodeIndex);
                if (existing == boneByNodeIndex.end())
                {
                    boneIndex = (int32_t)model.bones.size();
                    boneByNodeIndex[nodeIndex] = boneIndex;

                    KModelBone bone = {};
                    bone.name = model.nodes[nodeIndex].name;
                    bone.nodeIndex = nodeIndex;
                    model.bones.push_back(bone);
                }
                else
                {
                    boneIndex = existing->second;
                }

                if (skin.inverse_bind_matrices && jointIndex < skin.inverse_bind_matrices->count)
                {
                    float matrix[16] = {};
                    if (cgltf_accessor_read_float(skin.inverse_bind_matrices, jointIndex, matrix, 16))
                        std::memcpy(model.bones[boneIndex].inverseBindMatrix, matrix, sizeof(matrix));
                }
            }
        }

        for (auto& pair : boneByNodeIndex)
            model.nodes[pair.first].boneIndex = pair.second;

        for (KModelBone& bone : model.bones)
        {
            int32_t parentNodeIndex = model.nodes[bone.nodeIndex].parentIndex;
            while (parentNodeIndex >= 0)
            {
                auto found = boneByNodeIndex.find(parentNodeIndex);
                if (found != boneByNodeIndex.end())
                {
                    bone.parentBoneIndex = found->second;
                    break;
                }
                parentNodeIndex = model.nodes[parentNodeIndex].parentIndex;
            }
        }
    }

    static bool ImportAnimationChannels(const cgltf_animation& sourceAnimation, const cgltf_data& data, AnimationClipData& outClip, std::string& error)
    {
        std::unordered_map<int32_t, size_t> trackByNodeIndex;

        for (cgltf_size channelIndex = 0; channelIndex < sourceAnimation.channels_count; ++channelIndex)
        {
            const cgltf_animation_channel& channel = sourceAnimation.channels[channelIndex];
            if (!channel.target_node || !channel.sampler || !channel.sampler->input || !channel.sampler->output)
                continue;
            if (channel.target_path == cgltf_animation_path_type_weights)
                continue;

            const int32_t nodeIndex = PointerIndex(data.nodes, data.nodes_count, channel.target_node);
            if (nodeIndex < 0)
                continue;

            size_t trackIndex = 0;
            auto foundTrack = trackByNodeIndex.find(nodeIndex);
            if (foundTrack == trackByNodeIndex.end())
            {
                trackIndex = outClip.tracks.size();
                trackByNodeIndex[nodeIndex] = trackIndex;

                AnimationNodeTrack track = {};
                track.nodeIndex = nodeIndex;
                track.nodeName = channel.target_node->name ? channel.target_node->name : ("Node" + std::to_string(nodeIndex));
                outClip.tracks.push_back(track);
            }
            else
            {
                trackIndex = foundTrack->second;
            }

            const cgltf_accessor* input = channel.sampler->input;
            const cgltf_accessor* output = channel.sampler->output;
            const cgltf_size outputComponents = cgltf_num_components(output->type);
            if (outputComponents == 0)
                continue;

            const cgltf_size sampleCount = std::min(input->count, output->count);
            AnimationNodeTrack& track = outClip.tracks[trackIndex];
            float values[4] = {};

            if (channel.target_path == cgltf_animation_path_type_translation)
                track.translationKeys.reserve(track.translationKeys.size() + sampleCount);
            else if (channel.target_path == cgltf_animation_path_type_rotation)
                track.rotationKeys.reserve(track.rotationKeys.size() + sampleCount);
            else if (channel.target_path == cgltf_animation_path_type_scale)
                track.scaleKeys.reserve(track.scaleKeys.size() + sampleCount);

            for (cgltf_size sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
            {
                float time = 0.0f;
                if (!cgltf_accessor_read_float(input, sampleIndex, &time, 1))
                {
                    error = "Failed to read animation time";
                    return false;
                }
                if (!cgltf_accessor_read_float(output, sampleIndex, values, outputComponents))
                {
                    error = "Failed to read animation value";
                    return false;
                }

                outClip.duration = std::max(outClip.duration, time);

                if (channel.target_path == cgltf_animation_path_type_translation)
                {
                    AnimationVec3Key key = {};
                    key.time = time;
                    key.value[0] = values[0];
                    key.value[1] = values[1];
                    key.value[2] = values[2];
                    track.translationKeys.push_back(key);
                }
                else if (channel.target_path == cgltf_animation_path_type_rotation)
                {
                    AnimationQuatKey key = {};
                    key.time = time;
                    key.value[0] = values[0];
                    key.value[1] = values[1];
                    key.value[2] = values[2];
                    key.value[3] = outputComponents > 3 ? values[3] : 1.0f;
                    track.rotationKeys.push_back(key);
                }
                else if (channel.target_path == cgltf_animation_path_type_scale)
                {
                    AnimationVec3Key key = {};
                    key.time = time;
                    key.value[0] = values[0];
                    key.value[1] = values[1];
                    key.value[2] = values[2];
                    track.scaleKeys.push_back(key);
                }
            }
        }

        return true;
    }

    static bool SaveAnimations(const cgltf_data& data, const std::string& inputPath, const std::string& outputPath, std::string& error)
    {
        namespace fs = std::filesystem;

        const fs::path modelPath(outputPath);
        const fs::path outputDirectory = modelPath.parent_path();
        const std::string modelStem = modelPath.stem().string();
        std::unordered_map<std::string, size_t> clipNameUseCount;

        for (cgltf_size animationIndex = 0; animationIndex < data.animations_count; ++animationIndex)
        {
            const cgltf_animation& sourceAnimation = data.animations[animationIndex];
            AnimationClipData clip = {};
            clip.name = sourceAnimation.name ? sourceAnimation.name : ("Anim" + std::to_string(animationIndex));
            clip.sourcePath = inputPath;
            clip.modelPath = outputPath;
            clip.looping = true;

            if (!ImportAnimationChannels(sourceAnimation, data, clip, error))
                return false;
            if (clip.tracks.empty())
                continue;

            const std::string baseName = SanitizeName(clip.name, "Anim", 0);
            size_t& useCount = clipNameUseCount[baseName];
            const std::string fileName = modelStem + "_" + SanitizeName(clip.name, "Anim", useCount) + ".kanim";
            useCount++;

            const fs::path clipPath = outputDirectory / fileName;
            if (!SaveAnimationClipFile(clipPath.string().c_str(), clip, &error))
                return false;
        }

        return true;
    }
}

bool ImportGltfToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error)
{
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, inputPath.c_str(), &data);
    if (result != cgltf_result_success)
    {
        error = "cgltf_parse_file failed";
        return false;
    }

    result = cgltf_load_buffers(&options, data, inputPath.c_str());
    if (result != cgltf_result_success)
    {
        cgltf_free(data);
        error = "cgltf_load_buffers failed";
        return false;
    }

    result = cgltf_validate(data);
    if (result != cgltf_result_success)
    {
        cgltf_free(data);
        error = "cgltf_validate failed";
        return false;
    }

    KModelData model = {};
    model.sourcePath = inputPath;

    ImportNodes(*data, model);
    ImportBones(*data, model);
    if (!ImportMeshes(*data, model, error))
    {
        cgltf_free(data);
        return false;
    }

    const bool saved = SaveKModelFile(outputPath.c_str(), model, &error);
    if (saved && !SaveAnimations(*data, inputPath, outputPath, error))
    {
        cgltf_free(data);
        return false;
    }
    cgltf_free(data);
    return saved;
}

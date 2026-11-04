#include "kmodel.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <type_traits>

#include "binary_reader.h"
#include "pak_file_system.h"
#include "profiler.h"

namespace
{
    static constexpr uint32_t kKModelMagic = 0x4C444D4Bu; // KMDL
    static constexpr uint32_t kKModelVersion = 1u;

    // Vertices are read and written field by field in this order, without padding.
    static_assert(sizeof(KModelVertex) == 88 && std::is_trivially_copyable_v<KModelVertex>, "KModelVertex file layout");

    template<typename T>
    static bool WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return file.good();
    }

    static bool WriteString(std::ofstream& file, const std::string& value)
    {
        const uint32_t length = (uint32_t)value.size();
        if (!WriteValue(file, length))
            return false;
        file.write(value.data(), length);
        return file.good();
    }

    static bool WriteFloatArray(std::ofstream& file, const float* values, size_t count)
    {
        file.write(reinterpret_cast<const char*>(values), (std::streamsize)(sizeof(float) * count));
        return file.good();
    }

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

    static Float3 ApplyNodeTransformToPoint(const KModelNode& node, const Float3& point)
    {
        const Float3 scaled = {
            point.x * node.scale[0],
            point.y * node.scale[1],
            point.z * node.scale[2]
        };
        const Float3 rotated = RotateVectorByQuaternion(scaled, node.rotation);
        return {
            rotated.x + node.translation[0],
            rotated.y + node.translation[1],
            rotated.z + node.translation[2]
        };
    }

    static Float3 ApplyNodeTransformToDirection(const KModelNode& node, const Float3& direction)
    {
        const Float3 safeScale = {
            std::fabs(node.scale[0]) > 0.000001f ? node.scale[0] : 1.0f,
            std::fabs(node.scale[1]) > 0.000001f ? node.scale[1] : 1.0f,
            std::fabs(node.scale[2]) > 0.000001f ? node.scale[2] : 1.0f
        };
        const Float3 inverseScaled = {
            direction.x / safeScale.x,
            direction.y / safeScale.y,
            direction.z / safeScale.z
        };
        return Normalize(RotateVectorByQuaternion(inverseScaled, node.rotation));
    }

    static int32_t FindNodeForMesh(const KModelData& model, uint32_t meshIndex)
    {
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        {
            if (model.nodes[nodeIndex].meshIndex == (int32_t)meshIndex)
                return (int32_t)nodeIndex;
        }

        return -1;
    }

    static Float3 TransformPointByNodeHierarchy(const KModelData& model, int32_t nodeIndex, Float3 point)
    {
        int32_t currentNode = nodeIndex;
        while (currentNode >= 0 && currentNode < (int32_t)model.nodes.size())
        {
            point = ApplyNodeTransformToPoint(model.nodes[currentNode], point);
            currentNode = model.nodes[currentNode].parentIndex;
        }
        return point;
    }

    static Float3 TransformDirectionByNodeHierarchy(const KModelData& model, int32_t nodeIndex, Float3 direction)
    {
        int32_t currentNode = nodeIndex;
        while (currentNode >= 0 && currentNode < (int32_t)model.nodes.size())
        {
            direction = ApplyNodeTransformToDirection(model.nodes[currentNode], direction);
            currentNode = model.nodes[currentNode].parentIndex;
        }
        return Normalize(direction);
    }
}

void ConvertMeshDataToKModelMesh(const MeshData& mesh, KModelMesh& outMesh, const char* name)
{
    outMesh = {};
    if (name)
        outMesh.name = name;

    outMesh.vertices.reserve(mesh.vertices.size());
    for (const MeshVertex& vertex : mesh.vertices)
    {
        KModelVertex converted = {};
        converted.px = vertex.px;
        converted.py = vertex.py;
        converted.pz = vertex.pz;
        converted.nx = vertex.nx;
        converted.ny = vertex.ny;
        converted.nz = vertex.nz;
        converted.u = vertex.u;
        converted.v = vertex.v;
        converted.r = vertex.r;
        converted.g = vertex.g;
        converted.b = vertex.b;
        converted.a = vertex.a;
        outMesh.vertices.push_back(converted);
    }

    outMesh.indices = mesh.indices;
    if (!outMesh.indices.empty())
    {
        KModelPrimitive primitive = {};
        primitive.firstIndex = 0;
        primitive.indexCount = (uint32_t)outMesh.indices.size();
        outMesh.primitives.push_back(primitive);
    }
}

bool SaveKModelFile(const char* path, const KModelData& model, std::string* outError)
{
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing kmodel output path";
        return false;
    }

    const std::filesystem::path outputPath(path);
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path());

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open kmodel output file";
        return false;
    }

    if (!WriteValue(file, kKModelMagic) ||
        !WriteValue(file, kKModelVersion) ||
        !WriteString(file, model.sourcePath))
    {
        if (outError)
            *outError = "Failed to write kmodel header";
        return false;
    }

    const uint32_t materialCount = (uint32_t)model.materials.size();
    const uint32_t meshCount = (uint32_t)model.meshes.size();
    const uint32_t nodeCount = (uint32_t)model.nodes.size();
    const uint32_t boneCount = (uint32_t)model.bones.size();
    if (!WriteValue(file, materialCount) ||
        !WriteValue(file, meshCount) ||
        !WriteValue(file, nodeCount) ||
        !WriteValue(file, boneCount))
    {
        if (outError)
            *outError = "Failed to write kmodel counts";
        return false;
    }

    for (const KModelMaterial& material : model.materials)
    {
        if (!WriteString(file, material.name) ||
            !WriteString(file, material.materialPath))
        {
            if (outError)
                *outError = "Failed to write kmodel material";
            return false;
        }
    }

    for (const KModelMesh& mesh : model.meshes)
    {
        if (!WriteString(file, mesh.name))
        {
            if (outError)
                *outError = "Failed to write kmodel mesh name";
            return false;
        }

        const uint32_t vertexCount = (uint32_t)mesh.vertices.size();
        const uint32_t indexCount = (uint32_t)mesh.indices.size();
        const uint32_t primitiveCount = (uint32_t)mesh.primitives.size();
        if (!WriteValue(file, vertexCount) ||
            !WriteValue(file, indexCount) ||
            !WriteValue(file, primitiveCount))
        {
            if (outError)
                *outError = "Failed to write kmodel mesh counts";
            return false;
        }

        for (const KModelVertex& vertex : mesh.vertices)
        {
            if (!WriteValue(file, vertex.px) || !WriteValue(file, vertex.py) || !WriteValue(file, vertex.pz) ||
                !WriteValue(file, vertex.nx) || !WriteValue(file, vertex.ny) || !WriteValue(file, vertex.nz) ||
                !WriteValue(file, vertex.tx) || !WriteValue(file, vertex.ty) || !WriteValue(file, vertex.tz) || !WriteValue(file, vertex.tw) ||
                !WriteValue(file, vertex.u) || !WriteValue(file, vertex.v) ||
                !WriteValue(file, vertex.r) || !WriteValue(file, vertex.g) || !WriteValue(file, vertex.b) || !WriteValue(file, vertex.a))
            {
                if (outError)
                    *outError = "Failed to write kmodel vertex";
                return false;
            }

            file.write(reinterpret_cast<const char*>(vertex.boneIndices), sizeof(vertex.boneIndices));
            file.write(reinterpret_cast<const char*>(vertex.boneWeights), sizeof(vertex.boneWeights));
            if (!file.good())
            {
                if (outError)
                    *outError = "Failed to write kmodel skin weights";
                return false;
            }
        }

        if (!mesh.indices.empty())
        {
            file.write(reinterpret_cast<const char*>(mesh.indices.data()), (std::streamsize)(sizeof(uint32_t) * mesh.indices.size()));
            if (!file.good())
            {
                if (outError)
                    *outError = "Failed to write kmodel indices";
                return false;
            }
        }

        for (const KModelPrimitive& primitive : mesh.primitives)
        {
            if (!WriteValue(file, primitive.firstIndex) ||
                !WriteValue(file, primitive.indexCount) ||
                !WriteValue(file, primitive.materialIndex))
            {
                if (outError)
                    *outError = "Failed to write kmodel primitive";
                return false;
            }
        }
    }

    for (const KModelNode& node : model.nodes)
    {
        if (!WriteString(file, node.name) ||
            !WriteValue(file, node.parentIndex) ||
            !WriteValue(file, node.meshIndex) ||
            !WriteValue(file, node.boneIndex) ||
            !WriteFloatArray(file, node.translation, 3) ||
            !WriteFloatArray(file, node.rotation, 4) ||
            !WriteFloatArray(file, node.scale, 3))
        {
            if (outError)
                *outError = "Failed to write kmodel node";
            return false;
        }
    }

    for (const KModelBone& bone : model.bones)
    {
        if (!WriteString(file, bone.name) ||
            !WriteValue(file, bone.nodeIndex) ||
            !WriteValue(file, bone.parentBoneIndex) ||
            !WriteFloatArray(file, bone.inverseBindMatrix, 16))
        {
            if (outError)
                *outError = "Failed to write kmodel bone";
            return false;
        }
    }

    return true;
}

namespace
{
    // Everything later code indexes without checking.
    const char* FindKModelError(const KModelData& model)
    {
        const int32_t materialCount = (int32_t)model.materials.size();
        const int32_t meshCount = (int32_t)model.meshes.size();
        const int32_t nodeCount = (int32_t)model.nodes.size();
        const int32_t boneCount = (int32_t)model.bones.size();

        for (const KModelMesh& mesh : model.meshes)
        {
            for (uint32_t index : mesh.indices)
            {
                if (index >= mesh.vertices.size())
                    return "KModel index is out of range";
            }
            for (const KModelPrimitive& primitive : mesh.primitives)
            {
                if ((uint64_t)primitive.firstIndex + primitive.indexCount > mesh.indices.size())
                    return "KModel primitive is out of range";
                if (primitive.materialIndex < -1 || primitive.materialIndex >= materialCount)
                    return "KModel primitive references a missing material";
            }
        }

        for (int32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            const KModelNode& node = model.nodes[(size_t)nodeIndex];
            if (node.meshIndex < -1 || node.meshIndex >= meshCount)
                return "KModel node references a missing mesh";
            if (node.boneIndex < -1 || node.boneIndex >= boneCount)
                return "KModel node references a missing bone";

            int32_t steps = 0;
            for (int32_t parent = node.parentIndex; parent != -1; parent = model.nodes[(size_t)parent].parentIndex)
            {
                if (parent < -1 || parent >= nodeCount)
                    return "KModel node references a missing parent";
                if (++steps > nodeCount)
                    return "KModel node hierarchy has a cycle";
            }
        }

        for (const KModelBone& bone : model.bones)
        {
            if (bone.nodeIndex < 0 || bone.nodeIndex >= nodeCount)
                return "KModel bone references a missing node";
            if (bone.parentBoneIndex < -1 || bone.parentBoneIndex >= boneCount)
                return "KModel bone references a missing parent";
        }
        return nullptr;
    }
}

bool LoadKModelFile(const char* path, KModelData& outModel, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outModel = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;

    const auto fail = [&](const char* message)
    {
        if (outError)
            *outError = message;
        outModel = {};
        return false;
    };

    BinaryReader file(fileBytes);
    const uint32_t magic = file.u32();
    const uint32_t version = file.u32();
    if (!file.ok())
        return fail("Failed to read kmodel header");
    if (magic != kKModelMagic)
        return fail("Invalid kmodel magic");
    if (version != kKModelVersion)
        return fail("Unsupported kmodel version");

    outModel.sourcePath = file.string();
    const uint32_t materialCount = file.u32();
    const uint32_t meshCount = file.u32();
    const uint32_t nodeCount = file.u32();
    const uint32_t boneCount = file.u32();
    // Smallest possible size of each element in the file.
    if (!file.count(materialCount, 8) || !file.count(meshCount, 16) || !file.count(nodeCount, 56) || !file.count(boneCount, 76))
        return fail("Failed to read kmodel counts");

    outModel.materials.resize(materialCount);
    outModel.meshes.resize(meshCount);
    outModel.nodes.resize(nodeCount);
    outModel.bones.resize(boneCount);

    for (KModelMaterial& material : outModel.materials)
    {
        material.name = file.string();
        material.materialPath = file.string();
    }
    if (!file.ok())
        return fail("Failed to read kmodel material");

    for (KModelMesh& mesh : outModel.meshes)
    {
        mesh.name = file.string();
        const uint32_t vertexCount = file.u32();
        const uint32_t indexCount = file.u32();
        const uint32_t primitiveCount = file.u32();
        if (!file.count(vertexCount, sizeof(KModelVertex)))
            return fail("Failed to read kmodel mesh header");

        mesh.vertices.resize(vertexCount);
        file.bytes(mesh.vertices.data(), vertexCount * sizeof(KModelVertex));
        if (!file.count(indexCount, sizeof(uint32_t)))
            return fail("Failed to read kmodel indices");

        mesh.indices.resize(indexCount);
        file.bytes(mesh.indices.data(), indexCount * sizeof(uint32_t));
        if (!file.count(primitiveCount, 12))
            return fail("Failed to read kmodel primitives");

        mesh.primitives.resize(primitiveCount);
        for (KModelPrimitive& primitive : mesh.primitives)
        {
            primitive.firstIndex = file.u32();
            primitive.indexCount = file.u32();
            primitive.materialIndex = file.i32();
        }
    }

    for (KModelNode& node : outModel.nodes)
    {
        node.name = file.string();
        node.parentIndex = file.i32();
        node.meshIndex = file.i32();
        node.boneIndex = file.i32();
        file.bytes(node.translation, sizeof(node.translation));
        file.bytes(node.rotation, sizeof(node.rotation));
        file.bytes(node.scale, sizeof(node.scale));
    }
    if (!file.ok())
        return fail("Failed to read kmodel node");

    for (KModelBone& bone : outModel.bones)
    {
        bone.name = file.string();
        bone.nodeIndex = file.i32();
        bone.parentBoneIndex = file.i32();
        file.bytes(bone.inverseBindMatrix, sizeof(bone.inverseBindMatrix));
    }
    if (!file.ok())
        return fail("Failed to read kmodel bone");

    if (const char* error = FindKModelError(outModel))
        return fail(error);
    return true;
}

bool BuildMeshDataFromKModel(const KModelData& model, MeshData& outMesh, std::string* outError, uint32_t meshIndex)
{
    KES_ZONE_FN(ASSETS);
    outMesh = {};
    if (model.meshes.empty())
    {
        if (outError)
            *outError = "KModel has no meshes";
        return false;
    }

    const uint32_t firstMesh = meshIndex == KModelAllMeshes ? 0u : meshIndex;
    const uint32_t lastMesh = meshIndex == KModelAllMeshes ? (uint32_t)model.meshes.size() : (meshIndex + 1u);
    if (firstMesh >= model.meshes.size() || lastMesh > model.meshes.size())
    {
        if (outError)
            *outError = "Requested mesh index is out of range";
        return false;
    }

    size_t totalVertexCount = 0;
    size_t totalIndexCount = 0;
    for (uint32_t currentMesh = firstMesh; currentMesh < lastMesh; ++currentMesh)
    {
        totalVertexCount += model.meshes[currentMesh].vertices.size();
        totalIndexCount += model.meshes[currentMesh].indices.size();
    }

    outMesh.vertices.reserve(totalVertexCount);
    outMesh.indices.reserve(totalIndexCount);

    for (uint32_t currentMesh = firstMesh; currentMesh < lastMesh; ++currentMesh)
    {
        const KModelMesh& source = model.meshes[currentMesh];
        const int32_t nodeIndex = FindNodeForMesh(model, currentMesh);
        const uint32_t baseVertex = (uint32_t)outMesh.vertices.size();
        for (const KModelVertex& vertex : source.vertices)
        {
            MeshVertex converted = {};
            Float3 position = { vertex.px, vertex.py, vertex.pz };
            Float3 normal = { vertex.nx, vertex.ny, vertex.nz };
            if (nodeIndex >= 0)
            {
                position = TransformPointByNodeHierarchy(model, nodeIndex, position);
                normal = TransformDirectionByNodeHierarchy(model, nodeIndex, normal);
            }

            converted.px = position.x;
            converted.py = position.y;
            converted.pz = position.z;
            converted.nx = normal.x;
            converted.ny = normal.y;
            converted.nz = normal.z;
            converted.u = vertex.u;
            converted.v = vertex.v;
            converted.r = vertex.r;
            converted.g = vertex.g;
            converted.b = vertex.b;
            converted.a = vertex.a;
            outMesh.vertices.push_back(converted);
        }

        for (uint32_t index : source.indices)
            outMesh.indices.push_back(baseVertex + index);
    }

    if (outMesh.vertices.empty() || outMesh.indices.empty())
    {
        if (outError)
            *outError = "KModel mesh has no renderable geometry";
        return false;
    }

    return true;
}

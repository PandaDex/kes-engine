#include "kmodel.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "pak_file_system.h"
#include "profiler.h"

namespace
{
    static constexpr uint32_t kKModelMagic = 0x4C444D4Bu; // KMDL
    static constexpr uint32_t kKModelVersion = 1u;

    template<typename T>
    static bool WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return file.good();
    }

    template<typename T>
    static bool ReadValue(std::istream& file, T& value)
    {
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
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

    static bool ReadString(std::istream& file, std::string& value)
    {
        uint32_t length = 0;
        if (!ReadValue(file, length))
            return false;
        value.resize(length);
        if (length == 0)
            return true;
        file.read(value.data(), length);
        return file.good();
    }

    static bool WriteFloatArray(std::ofstream& file, const float* values, size_t count)
    {
        file.write(reinterpret_cast<const char*>(values), (std::streamsize)(sizeof(float) * count));
        return file.good();
    }

    static bool ReadFloatArray(std::istream& file, float* values, size_t count)
    {
        file.read(reinterpret_cast<char*>(values), (std::streamsize)(sizeof(float) * count));
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

bool LoadKModelFile(const char* path, KModelData& outModel, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outModel = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;

    const std::string fileData(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    std::istringstream file(fileData, std::ios::binary);

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!ReadValue(file, magic) ||
        !ReadValue(file, version) ||
        !ReadString(file, outModel.sourcePath))
    {
        if (outError)
            *outError = "Failed to read kmodel header";
        return false;
    }

    if (magic != kKModelMagic)
    {
        if (outError)
            *outError = "Invalid kmodel magic";
        return false;
    }

    if (version != kKModelVersion)
    {
        if (outError)
            *outError = "Unsupported kmodel version";
        return false;
    }

    uint32_t materialCount = 0;
    uint32_t meshCount = 0;
    uint32_t nodeCount = 0;
    uint32_t boneCount = 0;
    if (!ReadValue(file, materialCount) ||
        !ReadValue(file, meshCount) ||
        !ReadValue(file, nodeCount) ||
        !ReadValue(file, boneCount))
    {
        if (outError)
            *outError = "Failed to read kmodel counts";
        return false;
    }

    outModel.materials.resize(materialCount);
    outModel.meshes.resize(meshCount);
    outModel.nodes.resize(nodeCount);
    outModel.bones.resize(boneCount);

    for (KModelMaterial& material : outModel.materials)
    {
        if (!ReadString(file, material.name) ||
            !ReadString(file, material.materialPath))
        {
            if (outError)
                *outError = "Failed to read kmodel material";
            return false;
        }
    }

    for (KModelMesh& mesh : outModel.meshes)
    {
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t primitiveCount = 0;
        if (!ReadString(file, mesh.name) ||
            !ReadValue(file, vertexCount) ||
            !ReadValue(file, indexCount) ||
            !ReadValue(file, primitiveCount))
        {
            if (outError)
                *outError = "Failed to read kmodel mesh header";
            return false;
        }

        mesh.vertices.resize(vertexCount);
        mesh.indices.resize(indexCount);
        mesh.primitives.resize(primitiveCount);

        for (KModelVertex& vertex : mesh.vertices)
        {
            if (!ReadValue(file, vertex.px) || !ReadValue(file, vertex.py) || !ReadValue(file, vertex.pz) ||
                !ReadValue(file, vertex.nx) || !ReadValue(file, vertex.ny) || !ReadValue(file, vertex.nz) ||
                !ReadValue(file, vertex.tx) || !ReadValue(file, vertex.ty) || !ReadValue(file, vertex.tz) || !ReadValue(file, vertex.tw) ||
                !ReadValue(file, vertex.u) || !ReadValue(file, vertex.v) ||
                !ReadValue(file, vertex.r) || !ReadValue(file, vertex.g) || !ReadValue(file, vertex.b) || !ReadValue(file, vertex.a))
            {
                if (outError)
                    *outError = "Failed to read kmodel vertex";
                return false;
            }

            file.read(reinterpret_cast<char*>(vertex.boneIndices), sizeof(vertex.boneIndices));
            file.read(reinterpret_cast<char*>(vertex.boneWeights), sizeof(vertex.boneWeights));
            if (!file.good())
            {
                if (outError)
                    *outError = "Failed to read kmodel skin weights";
                return false;
            }
        }

        if (!mesh.indices.empty())
        {
            file.read(reinterpret_cast<char*>(mesh.indices.data()), (std::streamsize)(sizeof(uint32_t) * mesh.indices.size()));
            if (!file.good())
            {
                if (outError)
                    *outError = "Failed to read kmodel indices";
                return false;
            }
        }

        for (KModelPrimitive& primitive : mesh.primitives)
        {
            if (!ReadValue(file, primitive.firstIndex) ||
                !ReadValue(file, primitive.indexCount) ||
                !ReadValue(file, primitive.materialIndex))
            {
                if (outError)
                    *outError = "Failed to read kmodel primitive";
                return false;
            }
        }
    }

    for (KModelNode& node : outModel.nodes)
    {
        if (!ReadString(file, node.name) ||
            !ReadValue(file, node.parentIndex) ||
            !ReadValue(file, node.meshIndex) ||
            !ReadValue(file, node.boneIndex) ||
            !ReadFloatArray(file, node.translation, 3) ||
            !ReadFloatArray(file, node.rotation, 4) ||
            !ReadFloatArray(file, node.scale, 3))
        {
            if (outError)
                *outError = "Failed to read kmodel node";
            return false;
        }
    }

    for (KModelBone& bone : outModel.bones)
    {
        if (!ReadString(file, bone.name) ||
            !ReadValue(file, bone.nodeIndex) ||
            !ReadValue(file, bone.parentBoneIndex) ||
            !ReadFloatArray(file, bone.inverseBindMatrix, 16))
        {
            if (outError)
                *outError = "Failed to read kmodel bone";
            return false;
        }
    }

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

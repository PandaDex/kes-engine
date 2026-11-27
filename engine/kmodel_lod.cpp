#include "kmodel.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

#include "meshoptimizer.h"
#include "profiler.h"

namespace
{
    // Normal, tangent and UV, in KModelVertex order after the position.
    constexpr size_t kAttributeCount = 9;
    constexpr float kAttributeWeights[kAttributeCount] = { 0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };
    static_assert(offsetof(KModelVertex, u) - offsetof(KModelVertex, nx) == 7 * sizeof(float), "KModelVertex attribute layout");

    void SimplifyMesh(const KModelData& model, KModelMesh& mesh)
    {
        mesh.lods.clear();
        if (mesh.vertices.empty() || mesh.indices.empty())
            return;

        // Vertices shared by two primitives stay put so material borders don't crack.
        std::vector<unsigned char> lock(mesh.vertices.size(), 0);
        std::vector<int32_t> owner(mesh.vertices.size(), -1);
        for (size_t p = 0; p < mesh.primitives.size(); ++p)
        {
            const KModelPrimitive& primitive = mesh.primitives[p];
            for (uint32_t i = 0; i < primitive.indexCount; ++i)
            {
                const uint32_t vertex = mesh.indices[primitive.firstIndex + i];
                if (owner[vertex] >= 0 && owner[vertex] != (int32_t)p)
                    lock[vertex] = meshopt_SimplifyVertex_Lock;
                owner[vertex] = (int32_t)p;
            }
        }

        std::vector<KModelPrimitive> basePrimitives = mesh.primitives;
        if (basePrimitives.empty())
            basePrimitives.push_back({ 0, (uint32_t)mesh.indices.size(), -1 });

        const float* positions = &mesh.vertices[0].px;
        const float* attributes = &mesh.vertices[0].nx;
        std::vector<std::vector<uint32_t>> previous(basePrimitives.size());
        for (size_t p = 0; p < basePrimitives.size(); ++p)
            previous[p].assign(mesh.indices.begin() + basePrimitives[p].firstIndex,
                mesh.indices.begin() + basePrimitives[p].firstIndex + basePrimitives[p].indexCount);

        std::vector<uint32_t> simplified;
        for (const KModelLodLevel& level : model.lods)
        {
            KModelMeshLod lod;
            for (size_t p = 0; p < basePrimitives.size(); ++p)
            {
                const std::vector<uint32_t>& source = previous[p];
                const float ratio = std::clamp(level.triangleRatio, 0.0f, 1.0f);
                const size_t target = (size_t)((float)(basePrimitives[p].indexCount / 3) * ratio) * 3;
                simplified.resize(source.size());
                size_t count = source.empty() ? 0 : meshopt_simplifyWithAttributes(simplified.data(), source.data(), source.size(),
                    positions, mesh.vertices.size(), sizeof(KModelVertex), attributes, sizeof(KModelVertex),
                    kAttributeWeights, kAttributeCount, lock.data(), target, 1.0f, meshopt_SimplifyPrune, nullptr);
                simplified.resize(count);
                if (count > 0)
                    meshopt_optimizeVertexCache(simplified.data(), simplified.data(), count, mesh.vertices.size());

                lod.primitives.push_back({ (uint32_t)lod.indices.size(), (uint32_t)count, basePrimitives[p].materialIndex });
                lod.indices.insert(lod.indices.end(), simplified.begin(), simplified.end());
                previous[p] = simplified;
            }
            mesh.lods.push_back(std::move(lod));
        }
    }

    // "Body_LOD2" -> base "Body", level 2. Also accepts "Body.LOD2" and "Body LOD2", any case.
    bool ParseLodName(const std::string& name, std::string& outBase, uint32_t& outLevel)
    {
        size_t digits = name.size();
        while (digits > 0 && std::isdigit((unsigned char)name[digits - 1]))
            --digits;
        if (digits == name.size() || digits < 4)
            return false;
        const std::string tag = name.substr(digits - 3, 3);
        if (std::tolower((unsigned char)tag[0]) != 'l' || std::tolower((unsigned char)tag[1]) != 'o' || std::tolower((unsigned char)tag[2]) != 'd')
            return false;
        size_t end = digits - 3;
        if (end > 0 && (name[end - 1] == '_' || name[end - 1] == '.' || name[end - 1] == ' ' || name[end - 1] == '-'))
            --end;
        if (end == 0)
            return false;
        outLevel = (uint32_t)std::strtoul(name.c_str() + digits, nullptr, 10);
        outBase = name.substr(0, end);
        return true;
    }
}

void SetDefaultKModelLodLevels(KModelData& model, uint32_t count)
{
    model.lods.resize(count);
    float value = 1.0f;
    for (KModelLodLevel& level : model.lods)
    {
        value *= 0.5f;
        level.screenSize = value;
        level.triangleRatio = value;
    }
}

bool GenerateKModelLods(KModelData& model, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    for (const KModelLodLevel& level : model.lods)
    {
        if (!(level.triangleRatio >= 0.0f && level.triangleRatio <= 1.0f))
        {
            if (outError)
                *outError = "LOD triangle ratio must be between 0 and 1";
            return false;
        }
    }
    for (KModelMesh& mesh : model.meshes)
        SimplifyMesh(model, mesh);
    return true;
}

uint32_t ExtractKModelSourceLods(KModelData& model)
{
    struct Found
    {
        uint32_t mesh = 0;
        uint32_t base = 0;
        uint32_t level = 0;
    };
    // A mesh goes by its own name or the name of a node that uses it (Blender object names).
    std::vector<std::vector<std::string>> names(model.meshes.size());
    for (uint32_t index = 0; index < model.meshes.size(); ++index)
        names[index].push_back(model.meshes[index].name);
    for (const KModelNode& node : model.nodes)
    {
        if (node.meshIndex >= 0 && (size_t)node.meshIndex < names.size())
            names[(size_t)node.meshIndex].push_back(node.name);
    }
    const auto isBaseOf = [&](uint32_t mesh, const std::string& baseName)
    {
        for (const std::string& name : names[mesh])
        {
            std::string otherBase;
            uint32_t otherLevel = 0;
            if (name == baseName || (ParseLodName(name, otherBase, otherLevel) && otherLevel == 0 && otherBase == baseName))
                return true;
        }
        return false;
    };

    std::vector<Found> found;
    for (uint32_t index = 0; index < model.meshes.size(); ++index)
    {
        std::string baseName;
        uint32_t level = 0;
        bool isLod = false;
        for (const std::string& name : names[index])
        {
            if (ParseLodName(name, baseName, level) && level > 0)
            {
                isLod = true;
                break;
            }
        }
        if (!isLod)
            continue;
        for (uint32_t base = 0; base < model.meshes.size(); ++base)
        {
            if (base != index && isBaseOf(base, baseName))
            {
                found.push_back({ index, base, level });
                break;
            }
        }
    }
    if (found.empty())
        return 0;

    // LOD numbers can have gaps (LOD1, LOD3): they become consecutive levels.
    std::vector<uint32_t> levels;
    for (const Found& entry : found)
        levels.push_back(entry.level);
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    const size_t levelCount = levels.size();
    if (model.lods.size() != levelCount)
        SetDefaultKModelLodLevels(model, (uint32_t)levelCount);
    model.autoLods = false;

    for (KModelMesh& mesh : model.meshes)
    {
        mesh.lods.assign(levelCount, {});
        for (size_t level = 0; level < levelCount; ++level)
        {
            mesh.lods[level].indices = mesh.indices;
            mesh.lods[level].primitives = mesh.primitives;
        }
    }

    // Each LOD mesh's vertices are appended to its base mesh, so the LOD shares the base's vertex buffer.
    for (const Found& entry : found)
    {
        const size_t level = (size_t)(std::lower_bound(levels.begin(), levels.end(), entry.level) - levels.begin());
        const KModelMesh& source = model.meshes[entry.mesh];
        KModelMesh& base = model.meshes[entry.base];
        const uint32_t baseVertex = (uint32_t)base.vertices.size();
        base.vertices.insert(base.vertices.end(), source.vertices.begin(), source.vertices.end());
        KModelMeshLod& lod = base.lods[level];
        lod.indices.clear();
        for (uint32_t index : source.indices)
            lod.indices.push_back(baseVertex + index);
        lod.primitives = source.primitives;
        if (lod.primitives.empty())
            lod.primitives.push_back({ 0, (uint32_t)lod.indices.size(), -1 });
    }
    // A level only some meshes have: the others use their next coarser one, else LOD 0.
    for (KModelMesh& mesh : model.meshes)
    {
        std::vector<bool> has(levelCount, false);
        for (const Found& entry : found)
        {
            if (&model.meshes[entry.base] == &mesh)
                has[(size_t)(std::lower_bound(levels.begin(), levels.end(), entry.level) - levels.begin())] = true;
        }
        if (std::find(has.begin(), has.end(), true) == has.end())
        {
            mesh.lods.clear();
            continue;
        }
        for (size_t level = 1; level < levelCount; ++level)
        {
            if (!has[level] && has[level - 1])
            {
                mesh.lods[level] = mesh.lods[level - 1];
                has[level] = true;
            }
        }
    }

    std::vector<bool> removed(model.meshes.size(), false);
    for (const Found& entry : found)
        removed[entry.mesh] = true;
    std::vector<int32_t> remap(model.meshes.size(), -1);
    std::vector<KModelMesh> kept;
    for (uint32_t index = 0; index < model.meshes.size(); ++index)
    {
        if (removed[index])
            continue;
        remap[index] = (int32_t)kept.size();
        kept.push_back(std::move(model.meshes[index]));
    }
    model.meshes = std::move(kept);
    for (KModelNode& node : model.nodes)
    {
        if (node.meshIndex >= 0)
            node.meshIndex = remap[(size_t)node.meshIndex];
    }
    return (uint32_t)found.size();
}

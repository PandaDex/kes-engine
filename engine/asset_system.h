#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "mesh.h"
#include "kmodel.h"
#include "material.h"
#include "material_file.h"
#include "shader.h"
#include "animation_clip.h"
#include "animation_graph.h"

class AssetSystem
{
public:
    AssetSystem();

    MeshHandle CreateMesh(MeshData mesh);
    MeshHandle LoadMesh(const char* path);
    MeshHandle LoadObjMesh(const char* path);
    MeshHandle CreateCubeMesh();
    ModelHandle LoadModel(const char* path);

    ShaderHandle CreateShader(const ShaderData& shader);
    ShaderHandle LoadShader(const char* path);
    MaterialHandle CreateMaterial(const MaterialData& material);
    MaterialHandle LoadMaterial(const char* path);
    AnimationClipHandle LoadAnimationClip(const char* path);
    AnimationGraphHandle LoadAnimationGraph(const char* path);

    const MeshData* GetMesh(MeshHandle handle) const;
    const KModelData* GetModel(ModelHandle handle) const;
    const ShaderData* GetShader(ShaderHandle handle) const;
    const MaterialData* GetMaterial(MaterialHandle handle) const;
    const AnimationClipData* GetAnimationClip(AnimationClipHandle handle) const;
    const AnimationGraphData* GetAnimationGraph(AnimationGraphHandle handle) const;
    uint64_t GetGeneration() const { return generation; }

    // A file that fails to load is logged once and then returns 0 from the
    // cache until Clear(), so callers may retry freely.

    void Clear();

private:
    std::vector<MeshData> meshes;
    std::vector<KModelData> models;
    std::vector<ShaderData> shaders;
    std::vector<MaterialData> materials;
    std::vector<AnimationClipData> animationClips;
    std::vector<AnimationGraphData> animationGraphs;
    std::unordered_map<std::string, MeshHandle> meshFiles;
    std::unordered_map<std::string, ModelHandle> modelFiles;
    std::unordered_map<std::string, ShaderHandle> shaderFiles;
    std::unordered_map<std::string, MaterialHandle> materialFiles;
    std::unordered_map<std::string, AnimationClipHandle> animationClipFiles;
    std::unordered_map<std::string, AnimationGraphHandle> animationGraphFiles;
    uint64_t generation = 0;
};

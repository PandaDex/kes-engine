#include "asset_system.h"
#include "file_paths.h"
#include "animation_graph.h"
#include "mesh_file.h"
#include "material_file.h"
#include "shader.h"

AssetSystem::AssetSystem()
{
    Clear();
}

MeshHandle AssetSystem::CreateMesh(const MeshData& mesh)
{
    meshes.push_back(mesh);
    return (MeshHandle)(meshes.size() - 1);
}

MeshHandle AssetSystem::LoadMesh(const char* path)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = meshFiles.find(resolvedPath);
    if (cached != meshFiles.end())
        return cached->second;

    MeshData mesh;
    if (!LoadMeshFromFile(resolvedPath.c_str(), mesh))
        return 0;
    const MeshHandle handle = CreateMesh(mesh);
    meshFiles[resolvedPath] = handle;
    return handle;
}

MeshHandle AssetSystem::LoadObjMesh(const char* path)
{
    return LoadMesh(path);
}

MeshHandle AssetSystem::CreateCubeMesh()
{
    MeshData mesh;
    mesh.vertices = {
        { -1, -1, -1,  0,  0, -1, 0, 1, 1, 1, 1, 1 },
        {  1, -1, -1,  0,  0, -1, 1, 1, 1, 1, 1, 1 },
        {  1,  1, -1,  0,  0, -1, 1, 0, 1, 1, 1, 1 },
        { -1,  1, -1,  0,  0, -1, 0, 0, 1, 1, 1, 1 },

        { -1, -1,  1,  0,  0,  1, 0, 1, 1, 1, 1, 1 },
        {  1, -1,  1,  0,  0,  1, 1, 1, 1, 1, 1, 1 },
        {  1,  1,  1,  0,  0,  1, 1, 0, 1, 1, 1, 1 },
        { -1,  1,  1,  0,  0,  1, 0, 0, 1, 1, 1, 1 },

        { -1, -1,  1, -1,  0,  0, 0, 1, 1, 1, 1, 1 },
        { -1, -1, -1, -1,  0,  0, 1, 1, 1, 1, 1, 1 },
        { -1,  1, -1, -1,  0,  0, 1, 0, 1, 1, 1, 1 },
        { -1,  1,  1, -1,  0,  0, 0, 0, 1, 1, 1, 1 },

        {  1, -1,  1,  1,  0,  0, 0, 1, 1, 1, 1, 1 },
        {  1, -1, -1,  1,  0,  0, 1, 1, 1, 1, 1, 1 },
        {  1,  1, -1,  1,  0,  0, 1, 0, 1, 1, 1, 1 },
        {  1,  1,  1,  1,  0,  0, 0, 0, 1, 1, 1, 1 },

        { -1,  1, -1,  0,  1,  0, 0, 1, 1, 1, 1, 1 },
        {  1,  1, -1,  0,  1,  0, 1, 1, 1, 1, 1, 1 },
        {  1,  1,  1,  0,  1,  0, 1, 0, 1, 1, 1, 1 },
        { -1,  1,  1,  0,  1,  0, 0, 0, 1, 1, 1, 1 },

        { -1, -1, -1,  0, -1,  0, 0, 1, 1, 1, 1, 1 },
        {  1, -1, -1,  0, -1,  0, 1, 1, 1, 1, 1, 1 },
        {  1, -1,  1,  0, -1,  0, 1, 0, 1, 1, 1, 1 },
        { -1, -1,  1,  0, -1,  0, 0, 0, 1, 1, 1, 1 },
    };

    mesh.indices = {
         0, 1, 2,  0, 2, 3,
         4, 6, 5,  4, 7, 6,
         8, 9,10,  8,10,11,
        12,14,13, 12,15,14,
        16,17,18, 16,18,19,
        20,22,21, 20,23,22
    };

    return CreateMesh(mesh);
}

ModelHandle AssetSystem::LoadModel(const char* path)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = modelFiles.find(resolvedPath);
    if (cached != modelFiles.end())
        return cached->second;

    KModelData model = {};
    if (!LoadKModelFile(resolvedPath.c_str(), model))
        return 0;

    models.push_back(std::move(model));
    const ModelHandle handle = (ModelHandle)(models.size() - 1);
    modelFiles[resolvedPath] = handle;
    return handle;
}

ShaderHandle AssetSystem::CreateShader(const ShaderData& shader)
{
    shaders.push_back(shader);
    return (ShaderHandle)(shaders.size() - 1);
}

ShaderHandle AssetSystem::LoadShader(const char* path)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = shaderFiles.find(resolvedPath);
    if (cached != shaderFiles.end())
        return cached->second;

    ShaderData shader;
    if (!LoadShaderFile(resolvedPath.c_str(), shader))
        return 0;

    if (shader.name.empty())
        shader.name = resolvedPath;

    if (!shader.vertexShaderFile.empty())
        shader.vertexShaderFile = ResolveAssetPath(shader.vertexShaderFile.c_str());
    if (!shader.pixelShaderFile.empty())
        shader.pixelShaderFile = ResolveAssetPath(shader.pixelShaderFile.c_str());

    const ShaderHandle handle = CreateShader(shader);
    shaderFiles[resolvedPath] = handle;
    return handle;
}

MaterialHandle AssetSystem::CreateMaterial(const MaterialData& material)
{
    materials.push_back(material);
    return (MaterialHandle)(materials.size() - 1);
}

MaterialHandle AssetSystem::LoadMaterial(const char* path)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = materialFiles.find(resolvedPath);
    if (cached != materialFiles.end())
        return cached->second;

    MaterialFileData materialFile;
    if (!LoadMaterialFile(resolvedPath.c_str(), materialFile))
        return 0;

    EvaluateMaterialGraph(materialFile);
    if (!materialFile.material.baseTexturePath.empty())
        materialFile.material.baseTexturePath = ResolveAssetPath(materialFile.material.baseTexturePath.c_str());
    if (!materialFile.shaderPath.empty())
        materialFile.material.shader = LoadShader(materialFile.shaderPath.c_str());

    const MaterialHandle handle = CreateMaterial(materialFile.material);
    materialFiles[resolvedPath] = handle;
    return handle;
}

AnimationClipHandle AssetSystem::LoadAnimationClip(const char* path)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = animationClipFiles.find(resolvedPath);
    if (cached != animationClipFiles.end())
        return cached->second;

    AnimationClipData clip = {};
    if (!LoadAnimationClipFile(resolvedPath.c_str(), clip))
        return 0;

    animationClips.push_back(std::move(clip));
    const AnimationClipHandle handle = (AnimationClipHandle)(animationClips.size() - 1);
    animationClipFiles[resolvedPath] = handle;
    return handle;
}

AnimationGraphHandle AssetSystem::LoadAnimationGraph(const char* path)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = animationGraphFiles.find(resolvedPath);
    if (cached != animationGraphFiles.end())
        return cached->second;

    AnimationGraphData graph = {};
    if (!LoadAnimationGraphFile(resolvedPath.c_str(), graph))
        return 0;

    animationGraphs.push_back(std::move(graph));
    const AnimationGraphHandle handle = (AnimationGraphHandle)(animationGraphs.size() - 1);
    animationGraphFiles[resolvedPath] = handle;
    return handle;
}

const MeshData* AssetSystem::GetMesh(MeshHandle handle) const
{
    if (handle == 0 || handle >= meshes.size())
        return nullptr;
    return &meshes[(size_t)handle];
}

const KModelData* AssetSystem::GetModel(ModelHandle handle) const
{
    if (handle == 0 || handle >= models.size())
        return nullptr;
    return &models[(size_t)handle];
}

const ShaderData* AssetSystem::GetShader(ShaderHandle handle) const
{
    if (handle == 0 || handle >= shaders.size())
        return nullptr;
    return &shaders[(size_t)handle];
}

const MaterialData* AssetSystem::GetMaterial(MaterialHandle handle) const
{
    if (handle == 0 || handle >= materials.size())
        return nullptr;
    return &materials[(size_t)handle];
}

const AnimationClipData* AssetSystem::GetAnimationClip(AnimationClipHandle handle) const
{
    if (handle == 0 || handle >= animationClips.size())
        return nullptr;
    return &animationClips[(size_t)handle];
}

const AnimationGraphData* AssetSystem::GetAnimationGraph(AnimationGraphHandle handle) const
{
    if (handle == 0 || handle >= animationGraphs.size())
        return nullptr;
    return &animationGraphs[(size_t)handle];
}

void AssetSystem::Clear()
{
    ++generation;
    meshes.clear();
    models.clear();
    shaders.clear();
    materials.clear();
    animationClips.clear();
    animationGraphs.clear();
    meshFiles.clear();
    modelFiles.clear();
    shaderFiles.clear();
    materialFiles.clear();
    animationClipFiles.clear();
    animationGraphFiles.clear();
    meshes.resize(1);
    models.resize(1);
    shaders.resize(1);
    materials.resize(1);
    animationClips.resize(1);
    animationGraphs.resize(1);
}

#include "asset_system.h"
#include "file_paths.h"
#include "animation_graph.h"
#include "mesh_file.h"
#include "material_file.h"
#include "shader.h"
#include "logger.h"
#include "profiler.h"

AssetSystem::AssetSystem()
{
    Clear();
}

MeshHandle AssetSystem::CreateMesh(MeshData mesh)
{
    meshes.push_back(std::move(mesh));
    return (MeshHandle)(meshes.size() - 1);
}

MeshHandle AssetSystem::LoadMesh(const char* path)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = meshFiles.find(resolvedPath);
    if (cached != meshFiles.end())
        return cached->second;

    MeshData mesh;
    std::string error;
    if (!LoadMeshFromFile(resolvedPath.c_str(), mesh, &error))
    {
        EngineLogf("Failed to load mesh %s: %s", resolvedPath.c_str(), error.c_str());
        meshFiles[resolvedPath] = 0;
        return 0;
    }
    const MeshHandle handle = CreateMesh(std::move(mesh));
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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = modelFiles.find(resolvedPath);
    if (cached != modelFiles.end())
        return cached->second;

    KModelData model = {};
    std::string error;
    if (!LoadKModelFile(resolvedPath.c_str(), model, &error))
    {
        EngineLogf("Failed to load model %s: %s", resolvedPath.c_str(), error.c_str());
        modelFiles[resolvedPath] = 0;
        return 0;
    }

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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = shaderFiles.find(resolvedPath);
    if (cached != shaderFiles.end())
        return cached->second;

    ShaderData shader;
    std::string error;
    if (!LoadShaderFile(resolvedPath.c_str(), shader, &error))
    {
        EngineLogf("Failed to load shader %s: %s", resolvedPath.c_str(), error.c_str());
        shaderFiles[resolvedPath] = 0;
        return 0;
    }

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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = materialFiles.find(resolvedPath);
    if (cached != materialFiles.end())
        return cached->second;

    MaterialFileData materialFile;
    std::string error;
    if (!LoadMaterialFile(resolvedPath.c_str(), materialFile, &error))
    {
        EngineLogf("Failed to load material %s: %s", resolvedPath.c_str(), error.c_str());
        materialFiles[resolvedPath] = 0;
        return 0;
    }

    EvaluateMaterialGraph(materialFile);
    MaterialData& material = materialFile.material;
    for (std::string* texturePath : {&material.baseTexturePath, &material.normalTexturePath, &material.metallicRoughnessTexturePath,
                                     &material.occlusionTexturePath, &material.emissiveTexturePath})
    {
        if (!texturePath->empty())
            *texturePath = ResolveAssetPath(texturePath->c_str());
    }
    if (!materialFile.shaderPath.empty())
        materialFile.material.shader = LoadShader(materialFile.shaderPath.c_str());

    const MaterialHandle handle = CreateMaterial(materialFile.material);
    materialFiles[resolvedPath] = handle;
    return handle;
}

AnimationClipHandle AssetSystem::LoadAnimationClip(const char* path)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = animationClipFiles.find(resolvedPath);
    if (cached != animationClipFiles.end())
        return cached->second;

    AnimationClipData clip = {};
    std::string error;
    if (!LoadAnimationClipFile(resolvedPath.c_str(), clip, &error))
    {
        EngineLogf("Failed to load animation clip %s: %s", resolvedPath.c_str(), error.c_str());
        animationClipFiles[resolvedPath] = 0;
        return 0;
    }

    animationClips.push_back(std::move(clip));
    const AnimationClipHandle handle = (AnimationClipHandle)(animationClips.size() - 1);
    animationClipFiles[resolvedPath] = handle;
    return handle;
}

AnimationGraphHandle AssetSystem::LoadAnimationGraph(const char* path)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    const std::string resolvedPath = ResolveAssetPath(path);
    auto cached = animationGraphFiles.find(resolvedPath);
    if (cached != animationGraphFiles.end())
        return cached->second;

    AnimationGraphData graph = {};
    std::string error;
    if (!LoadAnimationGraphFile(resolvedPath.c_str(), graph, &error))
    {
        EngineLogf("Failed to load animation graph %s: %s", resolvedPath.c_str(), error.c_str());
        animationGraphFiles[resolvedPath] = 0;
        return 0;
    }

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
    KES_ZONE_FN(ASSETS);
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

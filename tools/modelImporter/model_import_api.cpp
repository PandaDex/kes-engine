#include "model_import_api.h"
#include <filesystem>
#include "fbx_importer.h"
#include "gltf_importer.h"
#include "kmodel.h"
#include "obj_importer.h"

namespace
{
    std::string Lowercase(std::string value)
    {
        for (char& ch : value)
        {
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch - 'A' + 'a');
        }
        return value;
    }

    bool ImportObjToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error)
    {
        MeshData mesh = {};
        std::vector<std::string> materialNames;
        if (!LoadObjMeshFromFile(inputPath.c_str(), mesh, &materialNames))
        {
            error = "Failed to load OBJ mesh";
            return false;
        }

        KModelData model = {};
        model.sourcePath = std::filesystem::path(inputPath).filename().string();
        for (const std::string& name : materialNames)
            model.materials.push_back({ name, {} });

        KModelMesh kmesh = {};
        ConvertMeshDataToKModelMesh(mesh, kmesh, std::filesystem::path(inputPath).stem().string().c_str());
        model.meshes.push_back(std::move(kmesh));

        KModelNode root = {};
        root.name = std::filesystem::path(inputPath).stem().string();
        root.meshIndex = 0;
        model.nodes.push_back(root);

        CarryOverKModelAttachments(outputPath.c_str(), model);
        return SaveKModelFile(outputPath.c_str(), model, &error);
    }
}

bool CanImportModelSource(const std::string& path)
{
    const std::string extension = Lowercase(std::filesystem::path(path).extension().string());
    return extension == ".obj" || extension == ".gltf" || extension == ".glb" || extension == ".fbx";
}

bool ImportModelToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error, const ModelImportOptions& options)
{
    if (!options.mesh && !options.animations)
    {
        error = "Nothing to import: both the mesh and the animations are turned off";
        return false;
    }

    const std::string extension = Lowercase(std::filesystem::path(inputPath).extension().string());
    if (extension == ".obj")
    {
        if (!options.mesh)
        {
            error = "OBJ files have no animations";
            return false;
        }
        return ImportObjToKModel(inputPath, outputPath, error);
    }
    if (extension == ".gltf" || extension == ".glb")
        return ImportGltfToKModel(inputPath, outputPath, error, options);
    if (extension == ".fbx")
        return ImportFbxToKModel(inputPath, outputPath, error, options);

    error = "Unsupported source format: " + extension;
    return false;
}

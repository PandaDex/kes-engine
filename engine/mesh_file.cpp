#include "mesh_file.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "file_paths.h"
#include "kmodel.h"

namespace
{
    static std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return (char)std::tolower(ch);
            });
        return value;
    }
}

bool LoadMeshFromFile(const char* path, MeshData& outMesh, std::string* outError)
{
    outMesh = {};

    const std::string resolvedPath = ResolveAssetPath(path);
    const std::string extension = Lowercase(std::filesystem::path(resolvedPath).extension().string());

    if (extension == ".kmodel")
    {
        KModelData model = {};
        if (!LoadKModelFile(resolvedPath.c_str(), model, outError)) {
            LoadKModelFile(ResolveAssetPath("engine/fallback.kmodel").c_str(), model, outError);
        }
        return BuildMeshDataFromKModel(model, outMesh, outError);
    }
    else {
        KModelData model = {};
        LoadKModelFile(ResolveAssetPath("engine/fallback.kmodel").c_str(), model, outError);
        return BuildMeshDataFromKModel(model, outMesh, outError);
    }

    if (outError)
        *outError = "Unsupported mesh format: " + extension;
    return false;
}

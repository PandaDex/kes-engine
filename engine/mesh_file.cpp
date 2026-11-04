#include "mesh_file.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "file_paths.h"
#include "kmodel.h"
#include "logger.h"
#include "profiler.h"

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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outMesh = {};

    const std::string resolvedPath = ResolveAssetPath(path);
    const std::string extension = Lowercase(std::filesystem::path(resolvedPath).extension().string());

    if (extension == ".kmodel")
    {
        KModelData model = {};
        std::string error;
        if (!LoadKModelFile(resolvedPath.c_str(), model, &error)) {
            EngineLogf("Mesh %s failed to load (%s); using the fallback model.", resolvedPath.c_str(), error.c_str());
            LoadKModelFile(ResolveAssetPath("engine/fallback.kmodel").c_str(), model, outError);
        }
        return BuildMeshDataFromKModel(model, outMesh, outError);
    }
    else {
        EngineLogf("Mesh %s is not a .kmodel; using the fallback model.", resolvedPath.c_str());
        KModelData model = {};
        LoadKModelFile(ResolveAssetPath("engine/fallback.kmodel").c_str(), model, outError);
        return BuildMeshDataFromKModel(model, outMesh, outError);
    }
}

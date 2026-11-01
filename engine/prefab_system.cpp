#include "prefab_system.h"
#include "file_paths.h"

void PrefabSystem::Clear()
{
    cache.clear();
}

const std::vector<LevelBlock>* PrefabSystem::LoadPrefab(const char* path, std::string* outError)
{
    const std::string resolvedPath = ResolveAssetPath(path);
    auto it = cache.find(resolvedPath);
    if (it != cache.end())
        return &it->second;

    std::vector<LevelBlock> blocks;
    if (!LoadLevelFile(resolvedPath.c_str(), blocks, outError))
        return nullptr;

    auto inserted = cache.emplace(resolvedPath, std::move(blocks));
    return &inserted.first->second;
}

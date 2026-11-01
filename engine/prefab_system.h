#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "level_file.h"

class PrefabSystem
{
public:
    void Clear();
    const std::vector<LevelBlock>* LoadPrefab(const char* path, std::string* outError = nullptr);

private:
    std::unordered_map<std::string, std::vector<LevelBlock>> cache;
};

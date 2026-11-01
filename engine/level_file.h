#pragma once
#include <string>
#include <vector>

enum class LevelBlockType
{
    Object,
    Entity,
    Collider,
    Prefab
};

struct LevelProperty
{
    std::string key;
    std::string value;
};

struct LevelBlock
{
    LevelBlockType type = LevelBlockType::Object;
    std::vector<LevelProperty> properties;
    int line = 0;
};

bool LoadLevelFile(const char* path, std::vector<LevelBlock>& outBlocks, std::string* outError = nullptr);
const char* GetLevelBlockTypeName(LevelBlockType type);
const std::string* FindLevelProperty(const LevelBlock& block, const char* key);
std::vector<std::string> FindLevelProperties(const LevelBlock& block, const char* key);

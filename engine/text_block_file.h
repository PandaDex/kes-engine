#pragma once
#include <string>
#include <vector>

struct TextBlockProperty
{
    std::string key;
    std::string value;
};

struct TextBlock
{
    std::string type;
    std::vector<TextBlockProperty> properties;
    int line = 0;
};

bool LoadTextBlockFile(const char* path, std::vector<TextBlock>& outBlocks, std::string* outError = nullptr);
const std::string* FindTextBlockProperty(const TextBlock& block, const char* key);

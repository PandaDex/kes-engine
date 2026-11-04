#pragma once
#include <string>
#include <vector>
#include "engine_types.h"
#include "material.h"

enum class MaterialNodeType
{
    Color,
    Scalar,
    Texture,
    Add,
    Multiply,
    Output
};

struct MaterialNode
{
    int id = 0;
    MaterialNodeType type = MaterialNodeType::Color;
    Vector2 position = {};
    float value[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string assetPath;
};

struct MaterialLink
{
    int fromNode = 0;
    int fromSlot = 0;
    int toNode = 0;
    int toSlot = 0;
};

struct MaterialFileData
{
    std::string shaderPath;
    uint32_t authorFlags = MaterialFlag_Lit;
    bool transparencySet = false;   // the file says Transparent = ..., which beats the alpha rule
    std::string legacyBaseTexturePath;
    MaterialData material;
    std::vector<MaterialNode> nodes;
    std::vector<MaterialLink> links;
};

bool LoadMaterialFile(const char* path, MaterialFileData& outMaterialFile, std::string* outError = nullptr);
bool SaveMaterialFile(const char* path, const MaterialFileData& materialFile, std::string* outError = nullptr);
bool EvaluateMaterialGraph(MaterialFileData& materialFile, std::string* outError = nullptr);

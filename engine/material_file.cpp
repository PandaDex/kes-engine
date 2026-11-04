#include "material_file.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include "file_paths.h"
#include "text_block_file.h"
#include "text_parse_utils.h"
#include "profiler.h"

namespace
{
    const char* GetNodeTypeName(MaterialNodeType type)
    {
        switch (type)
        {
        case MaterialNodeType::Color: return "Color";
        case MaterialNodeType::Scalar: return "Scalar";
        case MaterialNodeType::Texture: return "Texture";
        case MaterialNodeType::Add: return "Add";
        case MaterialNodeType::Multiply: return "Multiply";
        case MaterialNodeType::Output: return "Output";
        default: return "Color";
        }
    }

    MaterialNodeType ParseNodeType(const std::string& value)
    {
        const std::string cleaned = textparse::Unquote(value);
        if (cleaned == "Scalar") return MaterialNodeType::Scalar;
        if (cleaned == "Texture") return MaterialNodeType::Texture;
        if (cleaned == "Add") return MaterialNodeType::Add;
        if (cleaned == "Multiply") return MaterialNodeType::Multiply;
        if (cleaned == "Output") return MaterialNodeType::Output;
        return MaterialNodeType::Color;
    }

    struct EvalValue
    {
        float v[4] = { 0, 0, 0, 0 };
        std::string texturePath;
    };

    static EvalValue MakeScalar(float value)
    {
        EvalValue out = {};
        out.v[0] = value;
        out.v[1] = value;
        out.v[2] = value;
        out.v[3] = value;
        return out;
    }

    static EvalValue Add(const EvalValue& a, const EvalValue& b)
    {
        EvalValue out = {};
        for (int i = 0; i < 4; ++i) out.v[i] = a.v[i] + b.v[i];
        if (!a.texturePath.empty())
            out.texturePath = a.texturePath;
        else if (!b.texturePath.empty())
            out.texturePath = b.texturePath;
        return out;
    }

    static EvalValue Mul(const EvalValue& a, const EvalValue& b)
    {
        EvalValue out = {};
        for (int i = 0; i < 4; ++i) out.v[i] = a.v[i] * b.v[i];
        if (!a.texturePath.empty())
            out.texturePath = a.texturePath;
        else if (!b.texturePath.empty())
            out.texturePath = b.texturePath;
        return out;
    }
}

bool LoadMaterialFile(const char* path, MaterialFileData& outMaterialFile, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outMaterialFile = {};
    outMaterialFile.authorFlags = MaterialFlag_Lit | MaterialFlag_CastShadows;
    outMaterialFile.material.flags = outMaterialFile.authorFlags;

    std::vector<TextBlock> blocks;
    if (!LoadTextBlockFile(path, blocks, outError))
        return false;

    for (const TextBlock& block : blocks)
    {
        if (block.type == "Material")
        {
            if (const std::string* shader = FindTextBlockProperty(block, "Shader"))
                outMaterialFile.shaderPath = textparse::Unquote(*shader);
            if (const std::string* texture = FindTextBlockProperty(block, "BaseTexture"))
                outMaterialFile.legacyBaseTexturePath = textparse::Unquote(*texture);
            else if (const std::string* legacyTexture = FindTextBlockProperty(block, "Texture"))
                outMaterialFile.legacyBaseTexturePath = textparse::Unquote(*legacyTexture);
            if (const std::string* baseColor = FindTextBlockProperty(block, "BaseColor"))
                textparse::ParseColor4(*baseColor, outMaterialFile.material.baseColor, outMaterialFile.material.baseColor);
            if (const std::string* emissive = FindTextBlockProperty(block, "Emissive"))
            {
                float temp[4] = {
                    outMaterialFile.material.emissiveColor[0],
                    outMaterialFile.material.emissiveColor[1],
                    outMaterialFile.material.emissiveColor[2],
                    0.0f
                };
                textparse::ParseColor4(*emissive, temp, temp);
                outMaterialFile.material.emissiveColor[0] = temp[0];
                outMaterialFile.material.emissiveColor[1] = temp[1];
                outMaterialFile.material.emissiveColor[2] = temp[2];
            }
            // PBR (glTF metallic/roughness)
            MaterialData& m = outMaterialFile.material;
            auto readFloat = [&](const char* key, float& value)
            {
                if (const std::string* text = FindTextBlockProperty(block, key))
                    value = textparse::ParseFloat(*text, value);
            };
            auto readPath = [&](const char* key, std::string& value)
            {
                if (const std::string* text = FindTextBlockProperty(block, key))
                    value = textparse::Unquote(*text);
            };
            readFloat("Metallic", m.metallic);
            readFloat("Roughness", m.roughness);
            readFloat("NormalScale", m.normalScale);
            readFloat("OcclusionStrength", m.occlusionStrength);
            readFloat("EmissiveIntensity", m.emissiveIntensity);
            readPath("NormalTexture", m.normalTexturePath);
            readPath("MetallicRoughnessTexture", m.metallicRoughnessTexturePath);
            readPath("OcclusionTexture", m.occlusionTexturePath);
            readPath("EmissiveTexture", m.emissiveTexturePath);
            m.metallic = std::clamp(m.metallic, 0.0f, 1.0f);
            m.roughness = std::clamp(m.roughness, 0.02f, 1.0f);
            if (const std::string* castShadows = FindTextBlockProperty(block, "CastShadows"))
            {
                if (textparse::ParseBool(*castShadows, true))
                    outMaterialFile.authorFlags |= MaterialFlag_CastShadows;
                else
                    outMaterialFile.authorFlags &= ~MaterialFlag_CastShadows;
            }

            if (const std::string* alphaCutoff = FindTextBlockProperty(block, "AlphaCutoff"))
                outMaterialFile.material.alphaCutoff = textparse::ParseFloat(*alphaCutoff, outMaterialFile.material.alphaCutoff);
            if (const std::string* lit = FindTextBlockProperty(block, "Lit"))
            {
                if (!textparse::ParseBool(*lit, true))
                    outMaterialFile.authorFlags &= ~MaterialFlag_Lit;
            }
            if (const std::string* transparent = FindTextBlockProperty(block, "Transparent"))
            {
                outMaterialFile.transparencySet = true;
                if (textparse::ParseBool(*transparent, false))
                    outMaterialFile.authorFlags |= MaterialFlag_Transparent;
                else
                    outMaterialFile.authorFlags &= ~MaterialFlag_Transparent;
            }
            if (const std::string* doubleSided = FindTextBlockProperty(block, "DoubleSided"))
            {
                if (textparse::ParseBool(*doubleSided, false))
                    outMaterialFile.authorFlags |= MaterialFlag_DoubleSided;
                else
                    outMaterialFile.authorFlags &= ~MaterialFlag_DoubleSided;
            }

            outMaterialFile.material.flags = outMaterialFile.authorFlags;
            outMaterialFile.material.baseTexturePath = outMaterialFile.legacyBaseTexturePath;
        }
        else if (block.type == "Node")
        {
            MaterialNode node = {};
            if (const std::string* id = FindTextBlockProperty(block, "Id"))
                node.id = (int)textparse::ParseFloat(*id, 0.0f);
            if (const std::string* type = FindTextBlockProperty(block, "Type"))
                node.type = ParseNodeType(*type);
            if (const std::string* position = FindTextBlockProperty(block, "Position"))
                node.position = textparse::ParseVector2(*position, {});
            if (const std::string* value = FindTextBlockProperty(block, "Value"))
                textparse::ParseColor4(*value, node.value, node.value);
            if (const std::string* asset = FindTextBlockProperty(block, "Asset"))
                node.assetPath = textparse::Unquote(*asset);
            outMaterialFile.nodes.push_back(node);
        }
        else if (block.type == "Link")
        {
            MaterialLink link = {};
            if (const std::string* fromNode = FindTextBlockProperty(block, "FromNode"))
                link.fromNode = (int)textparse::ParseFloat(*fromNode, 0.0f);
            if (const std::string* fromSlot = FindTextBlockProperty(block, "FromSlot"))
                link.fromSlot = (int)textparse::ParseFloat(*fromSlot, 0.0f);
            if (const std::string* toNode = FindTextBlockProperty(block, "ToNode"))
                link.toNode = (int)textparse::ParseFloat(*toNode, 0.0f);
            if (const std::string* toSlot = FindTextBlockProperty(block, "ToSlot"))
                link.toSlot = (int)textparse::ParseFloat(*toSlot, 0.0f);
            outMaterialFile.links.push_back(link);
        }
    }

    return true;
}

bool SaveMaterialFile(const char* path, const MaterialFileData& materialFile, std::string* outError)
{
    const std::string resolvedPath = ResolveAssetWritePath(path);
    std::filesystem::create_directories(std::filesystem::path(resolvedPath).parent_path());
    std::ofstream file(resolvedPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open material file for save";
        return false;
    }

    file << "Material {\n";
    if (!materialFile.shaderPath.empty())
        file << "    Shader = " << textparse::Quote(materialFile.shaderPath) << "\n";
    if (!materialFile.material.baseTexturePath.empty())
        file << "    BaseTexture = " << textparse::Quote(materialFile.material.baseTexturePath) << "\n";
    file << "    BaseColor = \"" << materialFile.material.baseColor[0] << "," << materialFile.material.baseColor[1] << "," << materialFile.material.baseColor[2] << "," << materialFile.material.baseColor[3] << "\"\n";
    file << "    Emissive = \"" << materialFile.material.emissiveColor[0] << "," << materialFile.material.emissiveColor[1] << "," << materialFile.material.emissiveColor[2] << "\"\n";
    const MaterialData& m = materialFile.material;
    file << "    Metallic = " << m.metallic << "\n";
    file << "    Roughness = " << m.roughness << "\n";
    const auto writePath = [&file](const char* key, const std::string& value)
    {
        if (!value.empty())
            file << "    " << key << " = " << textparse::Quote(value) << "\n";
    };
    writePath("NormalTexture", m.normalTexturePath);
    writePath("MetallicRoughnessTexture", m.metallicRoughnessTexturePath);
    writePath("OcclusionTexture", m.occlusionTexturePath);
    writePath("EmissiveTexture", m.emissiveTexturePath);
    if (m.normalScale != 1.0f)
        file << "    NormalScale = " << m.normalScale << "\n";
    if (m.occlusionStrength != 1.0f)
        file << "    OcclusionStrength = " << m.occlusionStrength << "\n";
    if (m.emissiveIntensity != 1.0f)
        file << "    EmissiveIntensity = " << m.emissiveIntensity << "\n";
    file << "    AlphaCutoff = " << materialFile.material.alphaCutoff << "\n";
    file << "    Lit = " << (((materialFile.authorFlags & MaterialFlag_Lit) != 0) ? "true" : "false") << "\n";
    file << "    Transparent = " << (((materialFile.authorFlags & MaterialFlag_Transparent) != 0) ? "true" : "false") << "\n";
    file << "    DoubleSided = " << (((materialFile.authorFlags & MaterialFlag_DoubleSided) != 0) ? "true" : "false") << "\n";
    file << "    CastShadows = " << (((materialFile.authorFlags & MaterialFlag_CastShadows) != 0) ? "true" : "false") << "\n";
    file << "}\n";

    for (const MaterialNode& node : materialFile.nodes)
    {
        file << "\nNode {\n";
        file << "    Id = " << node.id << "\n";
        file << "    Type = " << textparse::Quote(GetNodeTypeName(node.type)) << "\n";
        file << "    Position = \"" << node.position.x << "," << node.position.y << "\"\n";
        file << "    Value = \"" << node.value[0] << "," << node.value[1] << "," << node.value[2] << "," << node.value[3] << "\"\n";
        if (!node.assetPath.empty())
            file << "    Asset = " << textparse::Quote(node.assetPath) << "\n";
        file << "}\n";
    }

    for (const MaterialLink& link : materialFile.links)
    {
        file << "\nLink {\n";
        file << "    FromNode = " << link.fromNode << "\n";
        file << "    FromSlot = " << link.fromSlot << "\n";
        file << "    ToNode = " << link.toNode << "\n";
        file << "    ToSlot = " << link.toSlot << "\n";
        file << "}\n";
    }

    return true;
}

bool EvaluateMaterialGraph(MaterialFileData& materialFile, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    materialFile.material.flags = materialFile.authorFlags;
    materialFile.material.baseTexturePath = materialFile.legacyBaseTexturePath;

    std::unordered_map<int, size_t> indexById;
    for (size_t i = 0; i < materialFile.nodes.size(); ++i)
        indexById[materialFile.nodes[i].id] = i;

    std::unordered_map<int, EvalValue> cache;
    std::unordered_map<int, bool> evaluating;

    std::function<EvalValue(int)> evalNode = [&](int nodeId) -> EvalValue
    {
        auto cached = cache.find(nodeId);
        if (cached != cache.end())
            return cached->second;

        auto indexIt = indexById.find(nodeId);
        if (indexIt == indexById.end())
            return {};

        if (evaluating[nodeId])
            return {};
        evaluating[nodeId] = true;

        const MaterialNode& node = materialFile.nodes[indexIt->second];
        EvalValue result = {};

        if (node.type == MaterialNodeType::Color)
        {
            for (int i = 0; i < 4; ++i) result.v[i] = node.value[i];
        }
        else if (node.type == MaterialNodeType::Scalar)
        {
            result = MakeScalar(node.value[0]);
        }
        else if (node.type == MaterialNodeType::Texture)
        {
            for (int i = 0; i < 4; ++i)
                result.v[i] = 1.0f;
            result.texturePath = node.assetPath;
        }
        else
        {
            EvalValue inputA = {};
            EvalValue inputB = {};
            for (const MaterialLink& link : materialFile.links)
            {
                if (link.toNode != nodeId)
                    continue;
                if (link.toSlot == 0)
                    inputA = evalNode(link.fromNode);
                else if (link.toSlot == 1)
                    inputB = evalNode(link.fromNode);
            }

            if (node.type == MaterialNodeType::Add)
                result = Add(inputA, inputB);
            else if (node.type == MaterialNodeType::Multiply)
                result = Mul(inputA, inputB);
        }

        evaluating[nodeId] = false;
        cache[nodeId] = result;
        return result;
    };

    for (const MaterialNode& node : materialFile.nodes)
    {
        if (node.type != MaterialNodeType::Output)
            continue;

        for (const MaterialLink& link : materialFile.links)
        {
            if (link.toNode != node.id)
                continue;

            const EvalValue value = evalNode(link.fromNode);
            if (link.toSlot == 0)
            {
                for (int i = 0; i < 4; ++i)
                    materialFile.material.baseColor[i] = value.v[i];
                if (!value.texturePath.empty())
                    materialFile.material.baseTexturePath = value.texturePath;
            }
            else if (link.toSlot == 1)
            {
                materialFile.material.emissiveColor[0] = value.v[0];
                materialFile.material.emissiveColor[1] = value.v[1];
                materialFile.material.emissiveColor[2] = value.v[2];
                if (!value.texturePath.empty())
                    materialFile.material.emissiveTexturePath = value.texturePath;
            }
            else if (link.toSlot == 2)
            {
                materialFile.material.baseColor[3] = value.v[0];
            }
            else if (link.toSlot == 3)
            {
                materialFile.material.alphaCutoff = value.v[0];
            }
        }
    }

    materialFile.material.alphaCutoff = std::clamp(materialFile.material.alphaCutoff, 0.0f, 1.0f);
    const bool transparent = materialFile.transparencySet ? (materialFile.authorFlags & MaterialFlag_Transparent) != 0
                                                          : materialFile.material.baseColor[3] < 0.999f;
    if (transparent)
        materialFile.material.flags |= MaterialFlag_Transparent;
    else
        materialFile.material.flags &= ~MaterialFlag_Transparent;

    (void)outError;
    return true;
}

#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class TexturePixelFormat : uint32_t
{
    RGBA8 = 1
};

enum TextureFileFlags : uint32_t
{
    TextureFileFlag_None = 0,
    TextureFileFlag_Srgb = 1 << 0,
    TextureFileFlag_ClampU = 1 << 1,
    TextureFileFlag_ClampV = 1 << 2,
    TextureFileFlag_PointFiltered = 1 << 3
};

struct TextureFileData
{
    std::string sourcePath;
    uint32_t width = 0;
    uint32_t height = 0;
    TexturePixelFormat format = TexturePixelFormat::RGBA8;
    uint32_t flags = TextureFileFlag_None;
    uint32_t mipCount = 1;
    std::vector<uint8_t> pixels;
};

bool LoadTextureFile(const char* path, TextureFileData& outTexture, std::string* outError = nullptr);
bool LoadKTexFile(const char* path, TextureFileData& outTexture, std::string* outError = nullptr);
bool SaveKTexFile(const char* path, const TextureFileData& texture, std::string* outError = nullptr);

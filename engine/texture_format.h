#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class TexturePixelFormat : uint32_t
{
    RGBA8 = 1,
    BC1 = 2, // RGB + 1-bit alpha, 8 bytes per block
    BC4 = 4, // R, 8 bytes per block
    BC5 = 5, // RG, 16 bytes per block
    BC7 = 6, // RGBA, 16 bytes per block
};

enum TextureFileFlags : uint32_t
{
    TextureFileFlag_None = 0,
    TextureFileFlag_Srgb = 1 << 0,
    TextureFileFlag_ClampU = 1 << 1,
    TextureFileFlag_ClampV = 1 << 2,
    TextureFileFlag_PointFiltered = 1 << 3,
    TextureFileFlag_MirrorU = 1 << 4,
    TextureFileFlag_MirrorV = 1 << 5,
    TextureFileFlag_NoAnisotropy = 1 << 6, // bilinear / trilinear filter
    TextureFileFlag_NoMipFilter = 1 << 7, // bilinear: nearest mip
    TextureFileFlag_NormalMap = 1 << 8, // tangent-space XY, Z rebuilt in the shader
};

enum class TextureCompression : uint32_t
{
    Default, // BC7, keeps alpha
    DefaultSmall, // BC1, half the size of BC7, 1-bit alpha
    NormalMap, // BC5, Z rebuilt in the shader
    Masks, // BC7, always linear
    Grayscale, // BC4, red channel
    UserInterface, // RGBA8
    Uncompressed, // RGBA8
};

enum class TextureMipGen : uint32_t
{
    Box, // 2x2 average (in linear space for sRGB, renormalized for normal maps)
    NoMipmaps,
};

enum class TextureFilter : uint32_t
{
    Default, // anisotropic, as set in the graphics options
    Nearest,
    Bilinear,
    Trilinear,
};

enum class TextureAddress : uint32_t
{
    Wrap,
    Clamp,
    Mirror,
};

struct TextureGroupSettings
{
    TextureCompression compression = TextureCompression::Default;
    bool srgb = true;
    TextureMipGen mipGen = TextureMipGen::Box;
    uint32_t maxSize = 0; // 0 = no limit
    int32_t lodBias = 0; // top mips dropped when cooking
    TextureFilter filter = TextureFilter::Default;
    TextureAddress addressU = TextureAddress::Wrap;
    TextureAddress addressV = TextureAddress::Wrap;
};

enum TextureSettingBits : uint32_t
{
    TextureSetting_Compression = 1 << 0,
    TextureSetting_Srgb = 1 << 1,
    TextureSetting_MipGen = 1 << 2,
    TextureSetting_MaxSize = 1 << 3,
    TextureSetting_LodBias = 1 << 4,
    TextureSetting_Filter = 1 << 5,
    TextureSetting_AddressU = 1 << 6,
    TextureSetting_AddressV = 1 << 7,
};

struct TextureSettings
{
    std::string group = "World";
    uint32_t overrides = 0; // TextureSettingBits
    TextureGroupSettings values; // used for the overridden fields only
};

struct TextureGroup
{
    std::string name;
    TextureGroupSettings settings;
};

std::vector<TextureGroup> DefaultTextureGroups();
void ParseTextureGroups(const std::string& text, std::vector<TextureGroup>& groups);
const TextureGroup* FindTextureGroup(const std::vector<TextureGroup>& groups, const std::string& name);
// An unknown group falls back to the first one.
TextureGroupSettings ResolveTextureSettings(const TextureSettings& settings, const std::vector<TextureGroup>& groups);
std::string GuessTextureGroup(const std::string& path);

std::string SerializeTextureSettings(const TextureSettings& settings);
TextureSettings ParseTextureSettings(const std::string& text);

const char* TextureCompressionName(TextureCompression value);
const char* TextureMipGenName(TextureMipGen value);
const char* TextureFilterName(TextureFilter value);
const char* TextureAddressName(TextureAddress value);
const char* TexturePixelFormatName(TexturePixelFormat value);
bool ParseTextureCompression(const std::string& name, TextureCompression& out);
bool ParseTextureMipGen(const std::string& name, TextureMipGen& out);
bool ParseTextureFilter(const std::string& name, TextureFilter& out);
bool ParseTextureAddress(const std::string& name, TextureAddress& out);

struct TextureFileData
{
    uint32_t version = 0;
    std::string sourcePath;
    uint32_t width = 0; // mip 0
    uint32_t height = 0;
    TexturePixelFormat format = TexturePixelFormat::RGBA8;
    uint32_t flags = TextureFileFlag_None;
    uint32_t mipCount = 1;
    std::vector<uint8_t> pixels; // all mips, mip 0 first

    // Version 2 and later
    TextureSettings settings;
    std::vector<uint8_t> sourceData; // editor only, stripped from packaged builds
};

// 0 = the default material sampler.
inline uint32_t TextureSamplerFlags(uint32_t flags)
{
    return flags & (TextureFileFlag_ClampU | TextureFileFlag_ClampV | TextureFileFlag_MirrorU | TextureFileFlag_MirrorV |
        TextureFileFlag_PointFiltered | TextureFileFlag_NoAnisotropy | TextureFileFlag_NoMipFilter);
}

bool IsBlockCompressed(TexturePixelFormat format);
uint32_t TextureBlockBytes(TexturePixelFormat format); // 4 (bytes per pixel) for RGBA8
uint32_t TextureMipWidth(uint32_t width, uint32_t mip);
uint64_t TextureMipByteSize(TexturePixelFormat format, uint32_t width, uint32_t height);
uint64_t TextureMipOffset(const TextureFileData& texture, uint32_t mip);
uint32_t TextureRowPitch(TexturePixelFormat format, uint32_t width);
bool ValidateTextureData(const TextureFileData& texture, std::string* outError);

bool ReadKTex(const uint8_t* data, size_t size, TextureFileData& outTexture, std::string* outError, bool keepSource = true);
bool WriteKTex(const TextureFileData& texture, std::vector<uint8_t>& outBytes, std::string* outError, bool stripSource = false);

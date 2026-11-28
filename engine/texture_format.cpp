#include "texture_format.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include "binary_reader.h"

namespace
{
    constexpr uint32_t kKTexMagic = 0x5845544Bu; // KTEX
    constexpr uint32_t kKTexVersion = 2u;

    std::string Lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return value;
    }

    std::string Trim(const std::string& value)
    {
        size_t begin = 0, end = value.size();
        while (begin < end && std::isspace((unsigned char)value[begin]))
            ++begin;
        while (end > begin && std::isspace((unsigned char)value[end - 1]))
            --end;
        return value.substr(begin, end - begin);
    }

    bool ParseBool(const std::string& value)
    {
        const std::string v = Lower(value);
        return v == "true" || v == "1" || v == "yes";
    }

    template<typename T, size_t N>
    bool ParseEnum(const std::string& name, const char* const (&names)[N], T& out)
    {
        const std::string wanted = Lower(name);
        for (size_t i = 0; i < N; ++i)
        {
            if (Lower(names[i]) == wanted)
            {
                out = static_cast<T>(i);
                return true;
            }
        }
        return false;
    }

    const char* const kCompressionNames[] = { "Default", "DefaultSmall", "NormalMap", "Masks", "Grayscale", "UserInterface", "Uncompressed" };
    const char* const kMipGenNames[] = { "Box", "NoMipmaps" };
    const char* const kFilterNames[] = { "Default", "Nearest", "Bilinear", "Trilinear" };
    const char* const kAddressNames[] = { "Wrap", "Clamp", "Mirror" };

    template<typename T, size_t N>
    const char* EnumName(T value, const char* const (&names)[N])
    {
        const size_t index = static_cast<size_t>(value);
        return index < N ? names[index] : "?";
    }

    uint32_t ApplySettingKey(const std::string& key, const std::string& value, TextureGroupSettings& s)
    {
        const std::string k = Lower(key);
        if (k == "compression")
            return ParseTextureCompression(value, s.compression) ? TextureSetting_Compression : 0;
        if (k == "srgb")
        {
            s.srgb = ParseBool(value);
            return TextureSetting_Srgb;
        }
        if (k == "mipgen")
            return ParseTextureMipGen(value, s.mipGen) ? TextureSetting_MipGen : 0;
        if (k == "maxsize")
        {
            s.maxSize = (uint32_t)std::max(0, std::atoi(value.c_str()));
            return TextureSetting_MaxSize;
        }
        if (k == "lodbias")
        {
            s.lodBias = std::max(0, std::atoi(value.c_str()));
            return TextureSetting_LodBias;
        }
        if (k == "filter")
            return ParseTextureFilter(value, s.filter) ? TextureSetting_Filter : 0;
        if (k == "addressu")
            return ParseTextureAddress(value, s.addressU) ? TextureSetting_AddressU : 0;
        if (k == "addressv")
            return ParseTextureAddress(value, s.addressV) ? TextureSetting_AddressV : 0;
        return 0;
    }

    void WriteSettingKeys(std::ostringstream& out, const TextureGroupSettings& s, uint32_t mask)
    {
        if (mask & TextureSetting_Compression)
            out << "Compression=" << TextureCompressionName(s.compression) << '\n';
        if (mask & TextureSetting_Srgb)
            out << "SRGB=" << (s.srgb ? "True" : "False") << '\n';
        if (mask & TextureSetting_MipGen)
            out << "MipGen=" << TextureMipGenName(s.mipGen) << '\n';
        if (mask & TextureSetting_MaxSize)
            out << "MaxSize=" << s.maxSize << '\n';
        if (mask & TextureSetting_LodBias)
            out << "LODBias=" << s.lodBias << '\n';
        if (mask & TextureSetting_Filter)
            out << "Filter=" << TextureFilterName(s.filter) << '\n';
        if (mask & TextureSetting_AddressU)
            out << "AddressU=" << TextureAddressName(s.addressU) << '\n';
        if (mask & TextureSetting_AddressV)
            out << "AddressV=" << TextureAddressName(s.addressV) << '\n';
    }

    template<typename Fn>
    void ForEachLine(const std::string& text, Fn fn)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;
            fn(line);
        }
    }

    template<typename T>
    void Append(std::vector<uint8_t>& out, const T& value)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }

    void AppendString(std::vector<uint8_t>& out, const std::string& value)
    {
        Append(out, (uint32_t)value.size());
        out.insert(out.end(), value.begin(), value.end());
    }

    bool Fail(std::string* outError, const char* message)
    {
        if (outError)
            *outError = message;
        return false;
    }
}

std::vector<TextureGroup> DefaultTextureGroups()
{
    std::vector<TextureGroup> groups;
    auto add = [&](const char* name, TextureCompression compression, bool srgb)
    {
        TextureGroup group;
        group.name = name;
        group.settings.compression = compression;
        group.settings.srgb = srgb;
        groups.push_back(group);
        return &groups.back().settings;
    };
    add("World", TextureCompression::Default, true);
    add("WorldNormalMap", TextureCompression::NormalMap, false);
    add("WorldMasks", TextureCompression::Masks, false);
    add("Grayscale", TextureCompression::Grayscale, false);
    add("Effects", TextureCompression::Default, true);
    TextureGroupSettings* ui = add("UI", TextureCompression::UserInterface, true);
    ui->mipGen = TextureMipGen::NoMipmaps;
    ui->filter = TextureFilter::Bilinear;
    ui->addressU = ui->addressV = TextureAddress::Clamp;
    TextureGroupSettings* pixel = add("Pixel", TextureCompression::Uncompressed, true);
    pixel->mipGen = TextureMipGen::NoMipmaps;
    pixel->filter = TextureFilter::Nearest;
    return groups;
}

void ParseTextureGroups(const std::string& text, std::vector<TextureGroup>& groups)
{
    TextureGroupSettings* current = nullptr;
    ForEachLine(text, [&](const std::string& line)
    {
        if (line.front() == '[' && line.back() == ']')
        {
            current = nullptr;
            const std::string section = Trim(line.substr(1, line.size() - 2));
            if (Lower(section.substr(0, 6)) != "group." || section.size() <= 6)
                return;
            const std::string name = Trim(section.substr(6));
            for (TextureGroup& group : groups)
            {
                if (Lower(group.name) == Lower(name))
                    current = &group.settings;
            }
            if (!current)
            {
                groups.push_back({ name, {} });
                current = &groups.back().settings;
            }
            return;
        }
        const size_t eq = line.find('=');
        if (current && eq != std::string::npos)
            ApplySettingKey(Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)), *current);
    });
}

const TextureGroup* FindTextureGroup(const std::vector<TextureGroup>& groups, const std::string& name)
{
    const std::string wanted = Lower(name);
    for (const TextureGroup& group : groups)
    {
        if (Lower(group.name) == wanted)
            return &group;
    }
    return nullptr;
}

TextureGroupSettings ResolveTextureSettings(const TextureSettings& settings, const std::vector<TextureGroup>& groups)
{
    const TextureGroup* group = FindTextureGroup(groups, settings.group);
    if (!group && !groups.empty())
        group = &groups.front();
    TextureGroupSettings out = group ? group->settings : TextureGroupSettings{};
    const TextureGroupSettings& v = settings.values;
    const uint32_t o = settings.overrides;
    if (o & TextureSetting_Compression) out.compression = v.compression;
    if (o & TextureSetting_Srgb) out.srgb = v.srgb;
    if (o & TextureSetting_MipGen) out.mipGen = v.mipGen;
    if (o & TextureSetting_MaxSize) out.maxSize = v.maxSize;
    if (o & TextureSetting_LodBias) out.lodBias = v.lodBias;
    if (o & TextureSetting_Filter) out.filter = v.filter;
    if (o & TextureSetting_AddressU) out.addressU = v.addressU;
    if (o & TextureSetting_AddressV) out.addressV = v.addressV;
    // Data compressions are never sRGB.
    if (out.compression == TextureCompression::NormalMap || out.compression == TextureCompression::Masks ||
        out.compression == TextureCompression::Grayscale)
        out.srgb = false;
    return out;
}

std::string GuessTextureGroup(const std::string& path)
{
    std::string lower = Lower(path);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    const size_t slash = lower.find_last_of('/');
    std::string stem = slash == std::string::npos ? lower : lower.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos)
        stem.resize(dot);

    auto endsWith = [&](const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        return stem.size() > n && stem.compare(stem.size() - n, n, suffix) == 0;
    };
    for (const char* suffix : { "_n", "_nrm", "_norm", "_normal", "_normalmap" })
    {
        if (endsWith(suffix))
            return "WorldNormalMap";
    }
    for (const char* suffix : { "_orm", "_arm", "_mr", "_rough", "_roughness", "_ao", "_occlusion", "_mask", "_masks", "_metal", "_metallic", "_metallicroughness" })
    {
        if (endsWith(suffix))
            return "WorldMasks";
    }
    if (lower.find("/ui/") != std::string::npos || lower.rfind("ui/", 0) == 0)
        return "UI";
    return "World";
}

std::string SerializeTextureSettings(const TextureSettings& settings)
{
    std::ostringstream out;
    out << "Group=" << settings.group << '\n';
    WriteSettingKeys(out, settings.values, settings.overrides);
    return out.str();
}

TextureSettings ParseTextureSettings(const std::string& text)
{
    TextureSettings settings;
    ForEachLine(text, [&](const std::string& line)
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        if (Lower(key) == "group")
            settings.group = value;
        else
            settings.overrides |= ApplySettingKey(key, value, settings.values);
    });
    return settings;
}

const char* TextureCompressionName(TextureCompression value) { return EnumName(value, kCompressionNames); }
const char* TextureMipGenName(TextureMipGen value) { return EnumName(value, kMipGenNames); }
const char* TextureFilterName(TextureFilter value) { return EnumName(value, kFilterNames); }
const char* TextureAddressName(TextureAddress value) { return EnumName(value, kAddressNames); }
bool ParseTextureCompression(const std::string& name, TextureCompression& out) { return ParseEnum(name, kCompressionNames, out); }
bool ParseTextureMipGen(const std::string& name, TextureMipGen& out) { return ParseEnum(name, kMipGenNames, out); }
bool ParseTextureFilter(const std::string& name, TextureFilter& out) { return ParseEnum(name, kFilterNames, out); }
bool ParseTextureAddress(const std::string& name, TextureAddress& out) { return ParseEnum(name, kAddressNames, out); }

const char* TexturePixelFormatName(TexturePixelFormat value)
{
    switch (value)
    {
    case TexturePixelFormat::RGBA8: return "RGBA8";
    case TexturePixelFormat::BC1: return "BC1";
    case TexturePixelFormat::BC4: return "BC4";
    case TexturePixelFormat::BC5: return "BC5";
    case TexturePixelFormat::BC7: return "BC7";
    }
    return "?";
}

bool IsBlockCompressed(TexturePixelFormat format)
{
    return format != TexturePixelFormat::RGBA8;
}

uint32_t TextureBlockBytes(TexturePixelFormat format)
{
    switch (format)
    {
    case TexturePixelFormat::RGBA8: return 4;
    case TexturePixelFormat::BC1:
    case TexturePixelFormat::BC4: return 8;
    case TexturePixelFormat::BC5:
    case TexturePixelFormat::BC7: return 16;
    }
    return 0;
}

uint32_t TextureMipWidth(uint32_t width, uint32_t mip)
{
    return std::max(width >> mip, 1u);
}

uint32_t TextureRowPitch(TexturePixelFormat format, uint32_t width)
{
    if (!IsBlockCompressed(format))
        return width * 4u;
    return ((width + 3u) / 4u) * TextureBlockBytes(format);
}

uint64_t TextureMipByteSize(TexturePixelFormat format, uint32_t width, uint32_t height)
{
    const uint64_t rows = IsBlockCompressed(format) ? (height + 3u) / 4u : height;
    return rows * TextureRowPitch(format, width);
}

uint64_t TextureMipOffset(const TextureFileData& texture, uint32_t mip)
{
    uint64_t offset = 0;
    for (uint32_t i = 0; i < mip; ++i)
        offset += TextureMipByteSize(texture.format, TextureMipWidth(texture.width, i), TextureMipWidth(texture.height, i));
    return offset;
}

bool ValidateTextureData(const TextureFileData& texture, std::string* outError)
{
    if (TextureBlockBytes(texture.format) == 0)
        return Fail(outError, "Unsupported texture pixel format");
    if (texture.width == 0 || texture.height == 0 || texture.mipCount == 0 || texture.mipCount > 32)
        return Fail(outError, "Invalid texture dimensions");
    if (texture.pixels.size() < TextureMipOffset(texture, texture.mipCount))
        return Fail(outError, "Texture pixel payload is too small");
    return true;
}

bool ReadKTex(const uint8_t* data, size_t size, TextureFileData& outTexture, std::string* outError, bool keepSource)
{
    outTexture = {};
    BinaryReader file(data, size);
    const uint32_t magic = file.u32();
    const uint32_t version = file.u32();
    if (!file.ok())
        return Fail(outError, "Failed to read texture header");
    if (magic != kKTexMagic)
        return Fail(outError, "Invalid texture magic");
    if (version < 1 || version > kKTexVersion)
        return Fail(outError, "Unsupported texture version");

    TextureFileData texture;
    texture.version = version;
    texture.sourcePath = file.string();
    texture.width = file.u32();
    texture.height = file.u32();
    texture.format = static_cast<TexturePixelFormat>(file.u32());
    texture.flags = file.u32();
    texture.mipCount = file.u32();
    const uint32_t pixelDataSize = file.u32();
    if (!file.ok() || !file.count(pixelDataSize, 1))
        return Fail(outError, "Failed to read texture header");
    texture.pixels.resize(pixelDataSize);
    file.bytes(texture.pixels.data(), pixelDataSize);

    if (version == 1)
    {
        // Version 1 stored mip 0 only, whatever mipCount says; mips were made at load time.
        texture.mipCount = 1;
        texture.settings.group = "World";
    }
    else
    {
        texture.settings = ParseTextureSettings(file.string());
        const uint32_t sourceSize = file.u32();
        if (!file.ok() || !file.count(sourceSize, 1))
            return Fail(outError, "Failed to read texture source payload");
        if (keepSource)
        {
            texture.sourceData.resize(sourceSize);
            file.bytes(texture.sourceData.data(), sourceSize);
        }
    }
    if (!ValidateTextureData(texture, outError))
        return false;
    outTexture = std::move(texture);
    return true;
}

bool WriteKTex(const TextureFileData& texture, std::vector<uint8_t>& outBytes, std::string* outError, bool stripSource)
{
    outBytes.clear();
    if (!ValidateTextureData(texture, outError))
        return false;
    const uint64_t pixelBytes = TextureMipOffset(texture, texture.mipCount);
    static const std::vector<uint8_t> kNoSource;
    const std::vector<uint8_t>& source = stripSource ? kNoSource : texture.sourceData;
    if (pixelBytes > UINT32_MAX || source.size() > UINT32_MAX)
        return Fail(outError, "Texture is too large");

    outBytes.reserve(64 + texture.sourcePath.size() + pixelBytes + source.size());
    Append(outBytes, kKTexMagic);
    Append(outBytes, kKTexVersion);
    AppendString(outBytes, texture.sourcePath);
    Append(outBytes, texture.width);
    Append(outBytes, texture.height);
    Append(outBytes, static_cast<uint32_t>(texture.format));
    Append(outBytes, texture.flags);
    Append(outBytes, texture.mipCount);
    Append(outBytes, static_cast<uint32_t>(pixelBytes));
    outBytes.insert(outBytes.end(), texture.pixels.begin(), texture.pixels.begin() + static_cast<ptrdiff_t>(pixelBytes));
    AppendString(outBytes, SerializeTextureSettings(texture.settings));
    Append(outBytes, static_cast<uint32_t>(source.size()));
    outBytes.insert(outBytes.end(), source.begin(), source.end());
    return true;
}

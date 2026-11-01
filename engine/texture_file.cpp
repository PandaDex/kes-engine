#include "texture_file.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <SDL3/SDL.h>
#include "file_paths.h"
#include "pak_file_system.h"

namespace
{
    static constexpr uint32_t kKTexMagic = 0x5845544Bu; // KTEX
    static constexpr uint32_t kKTexVersion = 1u;

    template<typename T>
    static bool WriteValue(std::ofstream& file, const T& value)
    {
        file.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return file.good();
    }

    template<typename T>
    static bool ReadValue(std::istream& file, T& value)
    {
        file.read(reinterpret_cast<char*>(&value), sizeof(T));
        return file.good();
    }

    static bool WriteString(std::ofstream& file, const std::string& value)
    {
        const uint32_t length = (uint32_t)value.size();
        if (!WriteValue(file, length))
            return false;
        if (length > 0)
            file.write(value.data(), length);
        return file.good();
    }

    static bool ReadString(std::istream& file, std::string& value)
    {
        uint32_t length = 0;
        if (!ReadValue(file, length))
            return false;
        value.resize(length);
        if (length > 0)
            file.read(value.data(), length);
        return file.good();
    }

    static std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return (char)std::tolower(ch);
            });
        return value;
    }

    static bool LoadBmpTexture(const std::string& resolvedPath, TextureFileData& outTexture, std::string* outError)
    {
        std::vector<uint8_t> fileBytes;
        if (!LoadAssetFileBytes(resolvedPath.c_str(), fileBytes, outError))
            return false;

        if (fileBytes.empty())
        {
            if (outError)
                *outError = "Texture file is empty";
            return false;
        }

        SDL_IOStream* rw = SDL_IOFromConstMem(fileBytes.data(), (int)fileBytes.size());
        if (!rw)
        {
            if (outError)
                *outError = "SDL_RWFromConstMem failed: " + std::string(SDL_GetError());
            return false;
        }

        SDL_Surface* loaded = SDL_LoadBMP_IO(rw, 1);
        if (!loaded)
        {
            if (outError)
                *outError = "SDL_LoadBMP_RW failed: " + std::string(SDL_GetError());
            return false;
        }

        SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(loaded);
        if (!converted)
        {
            if (outError)
                *outError = "SDL_ConvertSurfaceFormat failed: " + std::string(SDL_GetError());
            return false;
        }

        outTexture = {};
        outTexture.sourcePath = resolvedPath;
        outTexture.width = static_cast<uint32_t>(converted->w);
        outTexture.height = static_cast<uint32_t>(converted->h);
        outTexture.format = TexturePixelFormat::RGBA8;
        outTexture.flags = TextureFileFlag_Srgb;
        outTexture.mipCount = 1;

        const size_t rowBytes = static_cast<size_t>(converted->w) * 4u;
        outTexture.pixels.resize(static_cast<size_t>(converted->w) * static_cast<size_t>(converted->h) * 4u);
        const uint8_t* src = static_cast<const uint8_t*>(converted->pixels);
        for (int y = 0; y < converted->h; ++y)
        {
            std::memcpy(outTexture.pixels.data() + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * static_cast<size_t>(converted->pitch), rowBytes);
        }

        SDL_DestroySurface(converted);
        return true;
    }
}

bool LoadTextureFile(const char* path, TextureFileData& outTexture, std::string* outError)
{
    outTexture = {};
    const std::string resolvedPath = ResolveAssetPath(path);
    const std::string extension = Lowercase(std::filesystem::path(resolvedPath).extension().string());

    if (extension == ".ktex")
        return LoadKTexFile(resolvedPath.c_str(), outTexture, outError);

    if (extension == ".bmp")
        return LoadBmpTexture(resolvedPath, outTexture, outError);

    if (outError)
        *outError = "Unsupported texture format: " + extension + ". Import it to .ktex first.";
    return false;
}

bool LoadKTexFile(const char* path, TextureFileData& outTexture, std::string* outError)
{
    outTexture = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;

    const std::string fileData(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    std::istringstream file(fileData, std::ios::binary);

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t format = 0;
    uint32_t pixelDataSize = 0;
    if (!ReadValue(file, magic) ||
        !ReadValue(file, version) ||
        !ReadString(file, outTexture.sourcePath) ||
        !ReadValue(file, outTexture.width) ||
        !ReadValue(file, outTexture.height) ||
        !ReadValue(file, format) ||
        !ReadValue(file, outTexture.flags) ||
        !ReadValue(file, outTexture.mipCount) ||
        !ReadValue(file, pixelDataSize))
    {
        if (outError)
            *outError = "Failed to read texture header";
        return false;
    }

    if (magic != kKTexMagic)
    {
        if (outError)
            *outError = "Invalid texture magic";
        return false;
    }

    if (version != kKTexVersion)
    {
        if (outError)
            *outError = "Unsupported texture version";
        return false;
    }

    outTexture.format = static_cast<TexturePixelFormat>(format);
    if (outTexture.format != TexturePixelFormat::RGBA8)
    {
        if (outError)
            *outError = "Unsupported texture pixel format";
        return false;
    }

    if (outTexture.width == 0 || outTexture.height == 0 || outTexture.mipCount == 0)
    {
        if (outError)
            *outError = "Invalid texture dimensions";
        return false;
    }

    const uint64_t minimumSize = static_cast<uint64_t>(outTexture.width) * static_cast<uint64_t>(outTexture.height) * 4u;
    if (pixelDataSize < minimumSize)
    {
        if (outError)
            *outError = "Texture pixel payload is too small";
        return false;
    }

    outTexture.pixels.resize(pixelDataSize);
    if (pixelDataSize > 0)
        file.read(reinterpret_cast<char*>(outTexture.pixels.data()), pixelDataSize);
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to read texture pixel payload";
        return false;
    }

    return true;
}

bool SaveKTexFile(const char* path, const TextureFileData& texture, std::string* outError)
{
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing texture output path";
        return false;
    }

    if (texture.format != TexturePixelFormat::RGBA8)
    {
        if (outError)
            *outError = "Only RGBA8 textures are currently supported";
        return false;
    }

    const size_t expectedPixelBytes = static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 4u;
    if (texture.width == 0 || texture.height == 0 || texture.mipCount == 0 || texture.pixels.size() < expectedPixelBytes)
    {
        if (outError)
            *outError = "Texture data is incomplete";
        return false;
    }

    const std::filesystem::path outputPath(path);
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path());

    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open texture output file";
        return false;
    }

    const uint32_t format = static_cast<uint32_t>(texture.format);
    const uint32_t pixelDataSize = static_cast<uint32_t>(texture.pixels.size());
    if (!WriteValue(file, kKTexMagic) ||
        !WriteValue(file, kKTexVersion) ||
        !WriteString(file, texture.sourcePath) ||
        !WriteValue(file, texture.width) ||
        !WriteValue(file, texture.height) ||
        !WriteValue(file, format) ||
        !WriteValue(file, texture.flags) ||
        !WriteValue(file, texture.mipCount) ||
        !WriteValue(file, pixelDataSize))
    {
        if (outError)
            *outError = "Failed to write texture header";
        return false;
    }

    file.write(reinterpret_cast<const char*>(texture.pixels.data()), static_cast<std::streamsize>(texture.pixels.size()));
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write texture pixel payload";
        return false;
    }

    return true;
}

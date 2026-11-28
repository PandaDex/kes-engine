#include "texture_file.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <SDL3/SDL.h>
#include "file_paths.h"
#include "pak_file_system.h"
#include "profiler.h"

namespace
{
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
        outTexture.version = 1;

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
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
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

bool LoadKTexFile(const char* path, TextureFileData& outTexture, std::string* outError, bool keepSource)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outTexture = {};

    std::vector<uint8_t> fileBytes;
    if (!LoadAssetFileBytes(path, fileBytes, outError))
        return false;
    return ReadKTex(fileBytes.data(), fileBytes.size(), outTexture, outError, keepSource);
}

bool SaveKTexFile(const char* path, const TextureFileData& texture, std::string* outError)
{
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing texture output path";
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!WriteKTex(texture, bytes, outError))
        return false;

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
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write texture file";
        return false;
    }
    return true;
}

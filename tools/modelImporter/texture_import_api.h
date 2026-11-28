#pragma once
#include <string>

struct TextureImportOptions
{
    std::string group; // empty: keep the existing .ktex's settings, or guess from the file name
    std::string groupsPath;
};

bool CanImportTextureSource(const std::string& path);
bool ImportTextureToKTex(const std::string& inputPath, const std::string& outputPath, std::string& error,
    const TextureImportOptions& options = {});

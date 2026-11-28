#pragma once
#include <string>
#include "texture_format.h"

bool LoadTextureFile(const char* path, TextureFileData& outTexture, std::string* outError = nullptr);
bool LoadKTexFile(const char* path, TextureFileData& outTexture, std::string* outError = nullptr, bool keepSource = false);
bool SaveKTexFile(const char* path, const TextureFileData& texture, std::string* outError = nullptr);

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "texture_format.h"

TexturePixelFormat CookedTextureFormat(TextureCompression compression);

bool CookTexture(const uint8_t* rgba, uint32_t width, uint32_t height, const TextureGroupSettings& settings,
    TextureFileData& texture, std::string* outError = nullptr);

// One mip as RGBA8 (BC4 -> R,R,R,255 and BC5 -> R,G,rebuilt Z,255 for previews; see DecodeTextureMipRaw).
bool DecodeTextureMip(const TextureFileData& texture, uint32_t mip, std::vector<uint8_t>& outRgba);
// One mip as RGBA8 the way the GPU samples it: BC4 -> R,0,0,1, BC5 -> R,G,0,1.
bool DecodeTextureMipRaw(const TextureFileData& texture, uint32_t mip, std::vector<uint8_t>& outRgba);

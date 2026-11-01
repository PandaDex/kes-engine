#pragma once
#include <cstdint>
#include <string>
#include "shader.h"

using MaterialHandle = uint32_t;

enum MaterialFlags : uint32_t
{
    MaterialFlag_Lit = 1 << 0,
    MaterialFlag_Transparent = 1 << 1,
    MaterialFlag_DoubleSided = 1 << 2,
    MaterialFlag_Textured = 1 << 3
};

struct MaterialData
{
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveColor[3] = { 0.0f, 0.0f, 0.0f };
    float alphaCutoff = 0.0f;
    uint32_t flags = MaterialFlag_Lit;
    ShaderHandle shader = 0;
    std::string baseTexturePath;
};

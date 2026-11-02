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
    // Set by the renderer for the shader (which textures are bound), not by materials.
    MaterialFlag_HasBaseTexture = 1 << 3,
    MaterialFlag_HasNormalTexture = 1 << 4,
    MaterialFlag_HasMetallicRoughnessTexture = 1 << 5,
    MaterialFlag_HasOcclusionTexture = 1 << 6,
    MaterialFlag_HasEmissiveTexture = 1 << 7,
    MaterialFlag_CastShadows = 1 << 8,
};

// Metallic/roughness PBR material (glTF conventions). Textures:
//   base color, emissive        sRGB
//   normal                      tangent space, +Y up (OpenGL / glTF)
//   metallicRoughness           G = roughness, B = metallic (linear)
//   occlusion                   R (linear)
struct MaterialData
{
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveColor[3] = { 0.0f, 0.0f, 0.0f };
    float emissiveIntensity = 1.0f;
    float alphaCutoff = 0.0f;
    float metallic = 0.0f;
    float roughness = 0.8f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    uint32_t flags = MaterialFlag_Lit | MaterialFlag_CastShadows;
    ShaderHandle shader = 0;
    std::string baseTexturePath;
    std::string normalTexturePath;
    std::string metallicRoughnessTexturePath;
    std::string occlusionTexturePath;
    std::string emissiveTexturePath;
};

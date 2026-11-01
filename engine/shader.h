#pragma once
#include <cstdint>
#include <string>

using ShaderHandle = uint32_t;

// .shader asset. Stage files are content-relative .slang sources; the renderer
// loads the binaries compiled from them at build time (see shader_binary.h).
struct ShaderData
{
    std::string name;
    std::string vertexShaderFile;  // .slang with vsMain (empty = engine fallback)
    std::string pixelShaderFile;   // .slang with psMain (empty = engine fallback)
};

bool LoadShaderFile(const char* path, ShaderData& outShader, std::string* outError = nullptr);

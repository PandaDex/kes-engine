#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "render_backend.h"

// Precompiled shader lookup.
//
// Shaders are authored once in Slang (.slang) and compiled at build time by
// cmake/KesShaders.cmake into one binary per stage and backend, placed next to
// the source path inside the content directory:
//
//   shaders/foo.slang -> shaders/foo.vs.spv  / shaders/foo.ps.spv   (Vulkan)
//                        shaders/foo.vs.dxbc / shaders/foo.ps.dxbc  (D3D11)
//
// Entry points are vsMain (vertex) and psMain (pixel/fragment).

enum class ShaderStage
{
    Vertex,
    Pixel,
};

// Content-relative paths of the engine's built-in shaders.
inline constexpr const char* kEngineShaderMeshDefault = "engine/shaders/mesh_default.slang";
inline constexpr const char* kEngineShaderMeshFallback = "engine/shaders/mesh_fallback.slang";
inline constexpr const char* kEngineShaderSky = "engine/shaders/sky.slang";
inline constexpr const char* kEngineShaderShadow = "engine/shaders/shadow.slang";
inline constexpr const char* kEngineShaderBlit = "engine/shaders/blit.slang";
inline constexpr const char* kEngineShaderUi = "engine/shaders/ui.slang";

// "shaders/foo.slang" + Vertex + Vulkan -> "shaders/foo.vs.spv"
std::string BuildCompiledShaderPath(const std::string& sourcePath, ShaderStage stage, RendererBackend backend);

// Loads the compiled binary for sourcePath from disk or a mounted pak.
bool LoadCompiledShader(const std::string& sourcePath, ShaderStage stage, RendererBackend backend,
                        std::vector<uint8_t>& outBytes, std::string* outError = nullptr);

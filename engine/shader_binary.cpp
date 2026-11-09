#include "shader_binary.h"
#include "pak_file_system.h"

namespace
{
    const char* StageSuffix(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return "vs";
        case ShaderStage::Compute: return "cs";
        default: return "ps";
        }
    }

    const char* BackendExtension(RendererBackend backend)
    {
        switch (backend)
        {
        case RendererBackend::Vulkan: return "spv";
        case RendererBackend::D3D11:  return "dxbc";
        default:                      return "bin";
        }
    }
}

std::string BuildCompiledShaderPath(const std::string& sourcePath, ShaderStage stage, RendererBackend backend)
{
    std::string base = sourcePath;
    const size_t slash = base.find_last_of("/\\");
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base.erase(dot);

    base += '.';
    base += StageSuffix(stage);
    base += '.';
    base += BackendExtension(backend);
    return base;
}

bool LoadCompiledShader(const std::string& sourcePath, ShaderStage stage, RendererBackend backend,
                        std::vector<uint8_t>& outBytes, std::string* outError)
{
    const std::string path = BuildCompiledShaderPath(sourcePath, stage, backend);
    std::string error;
    if (!LoadAssetFileBytes(path.c_str(), outBytes, &error) || outBytes.empty())
    {
        if (outError)
            *outError = "Compiled shader not found: " + path + (error.empty() ? "" : " (" + error + ")") +
                        ". Shaders are compiled at build time from .slang sources; rebuild the project.";
        return false;
    }
    return true;
}

#include "shader.h"
#include "text_block_file.h"
#include "profiler.h"

namespace
{
    std::string Unquote(const std::string& value)
    {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            return value.substr(1, value.size() - 2);
        return value;
    }
}

bool LoadShaderFile(const char* path, ShaderData& outShader, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outShader = {};

    std::vector<TextBlock> blocks;
    if (!LoadTextBlockFile(path, blocks, outError))
        return false;

    if (blocks.empty())
    {
        if (outError)
            *outError = "Shader file is empty";
        return false;
    }

    const TextBlock& block = blocks[0];
    if (block.type != "Shader")
    {
        if (outError)
            *outError = "Shader file must start with a Shader block";
        return false;
    }

    if (const std::string* name = FindTextBlockProperty(block, "Name"))
        outShader.name = Unquote(*name);
    // Source = one .slang file providing both vsMain and psMain.
    // VertexShader / PixelShader override a single stage with another .slang file.
    if (const std::string* source = FindTextBlockProperty(block, "Source"))
    {
        outShader.vertexShaderFile = Unquote(*source);
        outShader.pixelShaderFile = outShader.vertexShaderFile;
    }
    if (const std::string* vs = FindTextBlockProperty(block, "VertexShader"))
        outShader.vertexShaderFile = Unquote(*vs);
    if (const std::string* ps = FindTextBlockProperty(block, "PixelShader"))
        outShader.pixelShaderFile = Unquote(*ps);

    if (outShader.vertexShaderFile.empty() && outShader.pixelShaderFile.empty())
    {
        if (outError)
            *outError = "Shader file does not define Source, VertexShader or PixelShader";
        return false;
    }

    return true;
}

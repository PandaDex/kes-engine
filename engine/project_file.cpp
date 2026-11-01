#include "project_file.h"
#include <filesystem>
#include <fstream>
#include "text_block_file.h"

namespace
{
    std::string UnquoteProjectValue(const std::string& value)
    {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            return value.substr(1, value.size() - 2);
        return value;
    }

    std::string QuoteProjectValue(const std::string& value)
    {
        return "\"" + value + "\"";
    }
}

bool LoadProjectFile(const char* path, ProjectFileData& outProject, std::string* outError)
{
    outProject = {};

    std::vector<TextBlock> blocks;
    if (!LoadTextBlockFile(path, blocks, outError))
        return false;

    if (blocks.empty())
    {
        if (outError)
            *outError = "Project file does not contain a Project block";
        return false;
    }

    const TextBlock* projectBlock = nullptr;
    for (const TextBlock& block : blocks)
    {
        if (block.type == "Project")
        {
            projectBlock = &block;
            break;
        }
    }

    if (!projectBlock)
    {
        if (outError)
            *outError = "Project file is missing a Project block";
        return false;
    }

    if (const std::string* value = FindTextBlockProperty(*projectBlock, "Name"))
        outProject.name = UnquoteProjectValue(*value);
    if (const std::string* value = FindTextBlockProperty(*projectBlock, "AssetRoot"))
        outProject.assetRoot = UnquoteProjectValue(*value);
    if (const std::string* value = FindTextBlockProperty(*projectBlock, "StartupMap"))
        outProject.startupMap = UnquoteProjectValue(*value);

    return true;
}

bool SaveProjectFile(const char* path, const ProjectFileData& project, std::string* outError)
{
    if (!path || !path[0])
    {
        if (outError)
            *outError = "Missing project file path";
        return false;
    }

    const std::filesystem::path outputPath(path);
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path());

    std::ofstream file(outputPath);
    if (!file.is_open())
    {
        if (outError)
            *outError = "Failed to open project file for writing";
        return false;
    }

    file << "Project\n{\n";
    file << "    Name=" << QuoteProjectValue(project.name) << "\n";
    file << "    AssetRoot=" << QuoteProjectValue(project.assetRoot) << "\n";
    file << "    StartupMap=" << QuoteProjectValue(project.startupMap) << "\n";
    file << "}\n";

    if (!file.good())
    {
        if (outError)
            *outError = "Failed to write project file";
        return false;
    }

    return true;
}

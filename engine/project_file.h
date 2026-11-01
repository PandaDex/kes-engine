#pragma once
#include <string>

struct ProjectFileData
{
    std::string name = "KES";
    std::string assetRoot = "assets";
    std::string startupMap = "maps/startup.map";
};

bool LoadProjectFile(const char* path, ProjectFileData& outProject, std::string* outError = nullptr);
bool SaveProjectFile(const char* path, const ProjectFileData& project, std::string* outError = nullptr);

#pragma once
#include <string>

struct ModelImportOptions
{
    bool mesh = true;
    bool animations = true;
};

bool CanImportModelSource(const std::string& path);
bool ImportModelToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error, const ModelImportOptions& options = {});

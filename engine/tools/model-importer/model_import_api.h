#pragma once
#include <string>

bool CanImportModelSource(const std::string& path);
bool ImportModelToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error);

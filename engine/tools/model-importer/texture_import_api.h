#pragma once
#include <string>

bool CanImportTextureSource(const std::string& path);
bool ImportTextureToKTex(const std::string& inputPath, const std::string& outputPath, std::string& error);

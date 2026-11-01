#pragma once

#include <string>

bool CanImportSoundSource(const std::string& path);
bool ImportSoundToKSound(const std::string& inputPath, const std::string& outputPath, std::string& error);

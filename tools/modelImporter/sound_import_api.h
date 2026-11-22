#pragma once

#include <string>

bool CanImportSoundSource(const std::string& path);
// compress: QOA (about a tenth of the size, near-transparent); otherwise float PCM.
bool ImportSoundToKSound(const std::string& inputPath, const std::string& outputPath, std::string& error, bool compress = true);

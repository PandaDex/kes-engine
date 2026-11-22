#pragma once
#include <string>
#include "model_import_api.h"

bool ImportFbxToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error, const ModelImportOptions& options = {});

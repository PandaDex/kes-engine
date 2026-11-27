#pragma once
#include <string>

struct KModelData;

struct ModelImportOptions
{
    bool mesh = true;
    bool animations = true;
    // Generated LODs: -1 keeps what the existing .kmodel had, 0 none, n that many (halving each).
    // Meshes named <name>_LOD<n> in the source are used instead when there are any.
    int autoLodCount = -1;
};

bool CanImportModelSource(const std::string& path);
// Source LODs, carried-over attachments and LOD generation, then the save. Every importer ends with it.
bool FinishAndSaveKModel(KModelData& model, const std::string& outputPath, const ModelImportOptions& options, std::string& error);
bool ImportModelToKModel(const std::string& inputPath, const std::string& outputPath, std::string& error, const ModelImportOptions& options = {});

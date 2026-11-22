#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "asset_file_io.h"
#include "engine.h"

void ClearMountedPakFiles();
void SetPakEncryptionKeyOverride(const char* hexKey);
std::string GetPakEncryptionKeyOverride();
bool MountPakFile(const char* pakPath, std::string* outError = nullptr);
bool MountContentPakFiles(std::string* outError = nullptr, String executablePath = "");
std::size_t GetMountedPakFileCount();
std::vector<std::string> GetMountedPakFilePaths();
// Files directly in a content directory, loose and in paks. Pak entries come
// back lower case. `extension` includes the dot.
std::vector<std::string> ListAssetFiles(const char* directory, const char* extension = nullptr);

std::string GeneratePakEncryptionKeyHex();
bool CreatePakFile(const char* sourceDirectory, const char* outputPath, const char* encryptionKeyHex, std::string* outError = nullptr);
bool ExtractPakFile(const char* pakPath, const char* outputDirectory, std::string* outError, const char* encryptionKeyHex = nullptr);

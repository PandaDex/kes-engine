#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine.h"

bool LoadAssetFileBytes(const char* path, std::vector<uint8_t>& outData, std::string* outError = nullptr);
bool LoadAssetFileText(const char* path, std::string& outText, std::string* outError = nullptr);

void ClearMountedPakFiles();
void SetPakEncryptionKeyOverride(const char* hexKey);
std::string GetPakEncryptionKeyOverride();
bool MountPakFile(const char* pakPath, std::string* outError = nullptr);
bool MountContentPakFiles(std::string* outError = nullptr,String executablePath = "");
std::size_t GetMountedPakFileCount();
std::vector<std::string> GetMountedPakFilePaths();

std::string GeneratePakEncryptionKeyHex();
bool CreatePakFile(const char* sourceDirectory, const char* outputPath, const char* encryptionKeyHex, std::string* outError = nullptr);
bool ExtractPakFile(const char* pakPath, const char* outputDirectory, std::string* outError);

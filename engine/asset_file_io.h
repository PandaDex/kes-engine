#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Reads loose content or a mounted pak. Kept apart from pak_file_system.h so
// tools can compile the asset loaders with their own implementation of these.
bool LoadAssetFileBytes(const char* path, std::vector<uint8_t>& outData, std::string* outError = nullptr);
bool LoadAssetFileText(const char* path, std::string& outText, std::string* outError = nullptr);

#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "engine_types.h"

#if PLATFORM_LINUX
#include <unistd.h>
#endif

#if PLATFORM_WINDOWS
#include <Windows.h>
#endif

#include <limits.h>

void SetAssetRootOverride(const char* path);
std::string GetAssetRootOverride();
std::string ResolveAssetPath(const char* path);
std::string ResolveAssetWritePath(const char* path);
int ApplyAssetRootOverrideArgument(int argc, const char* const* argv, int index);
std::vector<std::string> CollectPositionalCommandLineArguments(int argc, const char* const* argv, int firstIndex = 1);

static std::filesystem::path GetExecutableDirectory()
{
#if PLATFORM_WINDOWS
    char modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};

    std::filesystem::path path = std::filesystem::path(std::string(modulePath, length)).parent_path();

    return path.string();
#endif

#if PLATFORM_LINUX
    char modulePath[PATH_MAX] = {};

    const ssize_t length = readlink(
        "/proc/self/exe",
        modulePath,
        sizeof(modulePath) - 1
    );

    if (length <= 0)
        return {};

    modulePath[length] = '\0';

    const std::filesystem::path path(modulePath);

    return path.parent_path().string();
#endif
}

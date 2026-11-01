#include "platform_win.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include "../logger.h"

Vector2 platform_win::get_display_resolution(int displayIndex)
{
    if (displayIndex != 0) EngineLog("[platform_win::get_display_resolution] Display index is currently unused!");

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);

    return Vector2(static_cast<float>(width), static_cast<float>(height));
}

String platform_win::get_executable_path()
{
    char modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};

    std::filesystem::path path = std::filesystem::path(std::string(modulePath, length)).parent_path();

    return String(path.string());
}

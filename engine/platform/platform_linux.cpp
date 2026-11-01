#include "platform_linux.h"

#include <SDL3/SDL.h>
#include <unistd.h>
#include <limits.h>
#include <filesystem>
#include "../logger.h"

Vector2 platform_linux::get_display_resolution(int displayIndex)
{
    int displayCount = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);

    if (!displays || displayIndex < 0 || displayIndex >= displayCount)
    {
        EngineLog("[platform_linux::get_display_resolution] Invalid display index!");

        SDL_free(displays);
        return Vector2(0.0f, 0.0f);
    }

    const SDL_DisplayID displayID = displays[displayIndex];

    const SDL_DisplayMode* displayMode =
        SDL_GetCurrentDisplayMode(displayID);

    if (!displayMode)
    {
        EngineLog("[platform_linux::get_display_resolution] Failed to get display mode!");

        SDL_free(displays);
        return Vector2(0.0f, 0.0f);
    }

    const Vector2 resolution(
        static_cast<float>(displayMode->w),
        static_cast<float>(displayMode->h)
    );

    SDL_free(displays);

    return resolution;
}

String platform_linux::get_executable_path()
{
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

    return String(path.parent_path().string());
}
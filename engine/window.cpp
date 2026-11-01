#include "window.h"
#include <cstdio>
#include "logger.h"

bool CreateAppWindow(AppWindow& outWindow, const AppConfig& config, const char* title, SDL_WindowFlags extraFlags)
{
    outWindow.width = config.windowWidth;
    outWindow.height = config.windowHeight;

    outWindow.handle = SDL_CreateWindow(
        title,
        outWindow.width,
        outWindow.height,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | extraFlags
    );

    if (!outWindow.handle)
    {
        EngineLogf("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

void DestroyAppWindow(AppWindow& window)
{
    if (window.handle)
    {
        SDL_DestroyWindow(window.handle);
        window.handle = nullptr;
    }
}

void SetWindowSize(AppWindow& window, int width, int height)
{
    window.width = width;
    window.height = height;
}

void SetWindowTitle(AppWindow& window, const char* title)
{
    if (!window.handle || !title)
        return;

    SDL_SetWindowTitle(window.handle, title);
}

void ShowAppWindow(AppWindow& window)
{
    if (!window.handle)
        return;

    SDL_ShowWindow(window.handle);
}

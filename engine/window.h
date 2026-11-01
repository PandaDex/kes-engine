#pragma once
#include <SDL3/SDL.h>
#include "shared.h"

struct AppWindow
{
    SDL_Window* handle = nullptr;
    int width = 0;
    int height = 0;
};

// extraFlags carries backend specific flags (see GetRendererWindowFlags).
bool CreateAppWindow(AppWindow& outWindow, const AppConfig& config, const char* title, SDL_WindowFlags extraFlags = 0);
void DestroyAppWindow(AppWindow& window);
void SetWindowSize(AppWindow& window, int width, int height);
void SetWindowTitle(AppWindow& window, const char* title);
void ShowAppWindow(AppWindow& window);

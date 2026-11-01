#pragma once
#include <cstdint>
#include <vector>
#include <SDL3/SDL.h>
#include "shared.h"

// Application window: creation, window modes, display queries, focus /
// minimize tracking and mouse cursor state.

struct AppWindow
{
    SDL_Window* handle = nullptr;

    int width = 0;          // logical size (points)
    int height = 0;
    int pixelWidth = 0;     // drawable size in pixels (what the renderer presents to)
    int pixelHeight = 0;

    WindowMode mode = WindowMode::Windowed;
    int displayIndex = 0;
    IntPoint windowedSize = { 1280, 720 };   // restored when leaving fullscreen

    bool shown = false;
    bool minimized = false;
    bool occluded = false;
    bool focused = true;

    // Requested cursor state. Applied only while the window has focus; on
    // focus loss the cursor is always released so alt-tab works.
    bool cursorVisible = true;
    bool mouseLocked = false;   // relative mode: hidden, confined, raw deltas
};

struct WindowDesc
{
    const char* title = "kes";
    int width = 1280;
    int height = 720;
    WindowMode mode = WindowMode::Windowed;
    int displayIndex = 0;
    SDL_WindowFlags extraFlags = 0;   // backend flags, see GetRendererWindowFlags()
};

bool CreateAppWindow(AppWindow& outWindow, const WindowDesc& desc);
void DestroyAppWindow(AppWindow& window);
void ShowAppWindow(AppWindow& window);
void SetWindowTitle(AppWindow& window, const char* title);

// Switches window mode / resolution. For Fullscreen the closest supported
// display mode is used; for WindowedFullscreen the resolution is ignored.
bool SetAppWindowMode(AppWindow& window, WindowMode mode, IntPoint resolution, int displayIndex);

// True when there is something to render to (not minimized, non-zero size).
bool IsAppWindowRenderable(const AppWindow& window);

enum AppWindowEventFlags : uint32_t
{
    AppWindowEvent_None = 0,
    AppWindowEvent_PixelSizeChanged = 1u << 0,
    AppWindowEvent_FocusChanged = 1u << 1,
    AppWindowEvent_CloseRequested = 1u << 2,
    AppWindowEvent_ModeChanged = 1u << 3,
    AppWindowEvent_VisibilityChanged = 1u << 4,
};

// Updates window state from an SDL event; returns AppWindowEvent_* flags.
uint32_t AppWindowHandleEvent(AppWindow& window, const SDL_Event& event);

// Cursor
void SetAppWindowCursor(AppWindow& window, bool visible, bool locked);
void ApplyAppWindowCursor(AppWindow& window);   // re-apply after focus changes

// Displays (index 0 = primary)
int GetDisplayCount();
IntPoint GetDesktopResolution(int displayIndex);
std::vector<IntPoint> GetSupportedFullscreenResolutions(int displayIndex);

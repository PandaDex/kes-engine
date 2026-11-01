#include "window.h"
#include <algorithm>
#include "logger.h"

namespace
{
    constexpr int kMinimumWindowWidth = 320;
    constexpr int kMinimumWindowHeight = 240;

    const char* WindowModeName(WindowMode mode)
    {
        switch (mode)
        {
        case WindowMode::Fullscreen:         return "Fullscreen";
        case WindowMode::WindowedFullscreen: return "WindowedFullscreen";
        default:                             return "Windowed";
        }
    }

    SDL_DisplayID GetDisplayId(int displayIndex)
    {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        SDL_DisplayID id = 0;
        if (displays && count > 0)
            id = displays[(displayIndex >= 0 && displayIndex < count) ? displayIndex : 0];
        SDL_free(displays);
        return id ? id : SDL_GetPrimaryDisplay();
    }

    int GetDisplayIndex(SDL_DisplayID id)
    {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        int index = 0;
        for (int i = 0; displays && i < count; ++i)
        {
            if (displays[i] == id)
            {
                index = i;
                break;
            }
        }
        SDL_free(displays);
        return index;
    }

    void RefreshSizes(AppWindow& window)
    {
        if (!window.handle)
            return;
        SDL_GetWindowSize(window.handle, &window.width, &window.height);
        SDL_GetWindowSizeInPixels(window.handle, &window.pixelWidth, &window.pixelHeight);
    }
}

bool CreateAppWindow(AppWindow& outWindow, const WindowDesc& desc)
{
    outWindow = {};
    outWindow.mode = desc.mode;
    outWindow.displayIndex = desc.displayIndex;
    outWindow.windowedSize = { std::max(desc.width, kMinimumWindowWidth), std::max(desc.height, kMinimumWindowHeight) };

    const SDL_DisplayID display = GetDisplayId(desc.displayIndex);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, desc.title ? desc.title : "");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, outWindow.windowedSize.x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, outWindow.windowedSize.y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | desc.extraFlags);
    outWindow.handle = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);

    if (!outWindow.handle)
    {
        EngineLogf("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    SDL_SetWindowMinimumSize(outWindow.handle, kMinimumWindowWidth, kMinimumWindowHeight);

    if (desc.mode != WindowMode::Windowed)
        SetAppWindowMode(outWindow, desc.mode, { desc.width, desc.height }, desc.displayIndex);

    RefreshSizes(outWindow);
    return true;
}

void DestroyAppWindow(AppWindow& window)
{
    if (window.handle)
    {
        SDL_SetWindowRelativeMouseMode(window.handle, false);
        SDL_ShowCursor();
        SDL_DestroyWindow(window.handle);
        window.handle = nullptr;
    }
}

void ShowAppWindow(AppWindow& window)
{
    if (!window.handle)
        return;

    SDL_ShowWindow(window.handle);
    SDL_RaiseWindow(window.handle);
    window.shown = true;
    RefreshSizes(window);
    ApplyAppWindowCursor(window);
}

void SetWindowTitle(AppWindow& window, const char* title)
{
    if (window.handle && title)
        SDL_SetWindowTitle(window.handle, title);
}

bool SetAppWindowMode(AppWindow& window, WindowMode mode, IntPoint resolution, int displayIndex)
{
    if (!window.handle)
        return false;

    const SDL_DisplayID display = GetDisplayId(displayIndex);
    bool ok = true;

    switch (mode)
    {
    case WindowMode::Windowed:
    {
        const int width = std::max(resolution.x > 0 ? resolution.x : window.windowedSize.x, kMinimumWindowWidth);
        const int height = std::max(resolution.y > 0 ? resolution.y : window.windowedSize.y, kMinimumWindowHeight);
        ok &= SDL_SetWindowFullscreen(window.handle, false);
        SDL_SetWindowBordered(window.handle, true);
        SDL_SetWindowResizable(window.handle, true);
        SDL_SetWindowSize(window.handle, width, height);
        SDL_SetWindowPosition(window.handle, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        window.windowedSize = { width, height };
        break;
    }

    case WindowMode::WindowedFullscreen:
    {
        // Move onto the target display first; a NULL mode means "desktop fullscreen".
        SDL_Rect bounds = {};
        if (SDL_GetDisplayBounds(display, &bounds))
            SDL_SetWindowPosition(window.handle, bounds.x, bounds.y);
        ok &= SDL_SetWindowFullscreenMode(window.handle, nullptr);
        ok &= SDL_SetWindowFullscreen(window.handle, true);
        break;
    }

    case WindowMode::Fullscreen:
    {
        SDL_DisplayMode closest = {};
        const int width = resolution.x > 0 ? resolution.x : GetDesktopResolution(displayIndex).x;
        const int height = resolution.y > 0 ? resolution.y : GetDesktopResolution(displayIndex).y;
        SDL_Rect bounds = {};
        if (SDL_GetDisplayBounds(display, &bounds))
            SDL_SetWindowPosition(window.handle, bounds.x, bounds.y);
        if (SDL_GetClosestFullscreenDisplayMode(display, width, height, 0.0f, false, &closest))
        {
            ok &= SDL_SetWindowFullscreenMode(window.handle, &closest);
        }
        else
        {
            EngineLogf("No fullscreen mode close to %dx%d (%s); using desktop resolution.", width, height, SDL_GetError());
            ok &= SDL_SetWindowFullscreenMode(window.handle, nullptr);
        }
        ok &= SDL_SetWindowFullscreen(window.handle, true);
        break;
    }
    }

    // Window mode changes are asynchronous on some platforms; wait so the
    // sizes read below (and handed to the renderer) are final.
    SDL_SyncWindow(window.handle);

    window.mode = mode;
    window.displayIndex = displayIndex;
    RefreshSizes(window);

    if (!ok)
        EngineLogf("Window mode change to %s reported an error: %s", WindowModeName(mode), SDL_GetError());
    else
        EngineLogf("Window mode: %s %dx%d (pixels %dx%d) on display %d.", WindowModeName(mode),
                   window.width, window.height, window.pixelWidth, window.pixelHeight, displayIndex);
    return ok;
}

bool IsAppWindowRenderable(const AppWindow& window)
{
    return window.handle && !window.minimized && window.pixelWidth > 0 && window.pixelHeight > 0;
}

uint32_t AppWindowHandleEvent(AppWindow& window, const SDL_Event& event)
{
    if (!window.handle || event.type < SDL_EVENT_WINDOW_FIRST || event.type > SDL_EVENT_WINDOW_LAST)
        return AppWindowEvent_None;
    if (event.window.windowID != SDL_GetWindowID(window.handle))
        return AppWindowEvent_None;

    switch (event.type)
    {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        RefreshSizes(window);
        return AppWindowEvent_PixelSizeChanged;

    case SDL_EVENT_WINDOW_RESIZED:
        RefreshSizes(window);
        if (window.mode == WindowMode::Windowed && !window.minimized)
            window.windowedSize = { window.width, window.height };
        return AppWindowEvent_None;

    case SDL_EVENT_WINDOW_MINIMIZED:
        window.minimized = true;
        return AppWindowEvent_VisibilityChanged;

    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        window.minimized = false;
        RefreshSizes(window);
        return AppWindowEvent_VisibilityChanged | AppWindowEvent_PixelSizeChanged;

    case SDL_EVENT_WINDOW_OCCLUDED:
        window.occluded = true;
        return AppWindowEvent_VisibilityChanged;

    case SDL_EVENT_WINDOW_EXPOSED:
        window.occluded = false;
        return AppWindowEvent_VisibilityChanged;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        window.focused = true;
        ApplyAppWindowCursor(window);
        return AppWindowEvent_FocusChanged;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        window.focused = false;
        ApplyAppWindowCursor(window);
        return AppWindowEvent_FocusChanged;

    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        RefreshSizes(window);
        return AppWindowEvent_ModeChanged | AppWindowEvent_PixelSizeChanged;

    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        window.displayIndex = GetDisplayIndex(SDL_GetDisplayForWindow(window.handle));
        RefreshSizes(window);
        return AppWindowEvent_PixelSizeChanged;

    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        return AppWindowEvent_CloseRequested;

    default:
        return AppWindowEvent_None;
    }
}

void SetAppWindowCursor(AppWindow& window, bool visible, bool locked)
{
    window.cursorVisible = visible;
    window.mouseLocked = locked;
    ApplyAppWindowCursor(window);
}

void ApplyAppWindowCursor(AppWindow& window)
{
    if (!window.handle)
        return;

    // Never hold the mouse hostage while the window isn't focused or shown.
    const bool active = window.focused && window.shown && !window.minimized;
    const bool lock = active && window.mouseLocked;
    const bool visible = !active || (window.cursorVisible && !window.mouseLocked);

    SDL_SetWindowRelativeMouseMode(window.handle, lock);
    if (visible)
        SDL_ShowCursor();
    else
        SDL_HideCursor();
}

int GetDisplayCount()
{
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    SDL_free(displays);
    return count;
}

IntPoint GetDesktopResolution(int displayIndex)
{
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(GetDisplayId(displayIndex));
    if (!mode)
        return { 1920, 1080 };
    return { mode->w, mode->h };
}

std::vector<IntPoint> GetSupportedFullscreenResolutions(int displayIndex)
{
    std::vector<IntPoint> result;
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(GetDisplayId(displayIndex), &count);
    for (int i = 0; modes && i < count; ++i)
    {
        const IntPoint size = { modes[i]->w, modes[i]->h };
        if (std::find(result.begin(), result.end(), size) == result.end())
            result.push_back(size);
    }
    SDL_free(modes);

    // Largest first, like the display mode list.
    std::sort(result.begin(), result.end(), [](const IntPoint& a, const IntPoint& b)
    {
        return a.x != b.x ? a.x > b.x : a.y > b.y;
    });
    return result;
}

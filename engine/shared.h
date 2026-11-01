#pragma once

// Renderer backends. Which ones exist in a build is decided at configure time
// (see KES_RENDERERS_<PLATFORM> in engine/CMakeLists.txt).
enum class RendererBackend
{
    Auto = 0,   // platform's preferred backend, falling back in priority order
    Vulkan,
    D3D11,
};

// Window modes, same meaning and values as Unreal's EWindowMode.
enum class WindowMode
{
    Fullscreen = 0,          // exclusive fullscreen, changes the display mode
    WindowedFullscreen = 1,  // borderless window covering the display at desktop resolution
    Windowed = 2,            // regular resizable window
};

struct IntPoint
{
    int x = 0;
    int y = 0;

    bool operator==(const IntPoint& other) const { return x == other.x && y == other.y; }
    bool operator!=(const IntPoint& other) const { return !(*this == other); }
};

// Startup defaults. Player-facing values (resolution, window mode, vsync, ...)
// are owned by GameSettings and only fall back to these when no settings
// file exists yet.
struct AppConfig
{
    int windowWidth = 1280;
    int windowHeight = 720;
    WindowMode windowMode = WindowMode::Windowed;

    // Base internal render resolution. GameSettings' resolution scale is applied on top.
    int renderWidth = 1920;
    int renderHeight = 1080;

    bool preservePresentationAspect = false;
    bool preferIntegerPresentationScale = true;

    bool vsync = true;
    float frameRateLimit = 0.0f;   // 0 = unlimited

    // Preferred renderer. Ignored when the platform forces a single backend.
    // Overridden by "-renderer=<name>" on the command line or KES_RENDERER env var.
    RendererBackend renderer = RendererBackend::Auto;
};

#pragma once

// Renderer backends. Which ones exist in a build is decided at configure time
// (see KES_RENDERERS_<PLATFORM> in engine/CMakeLists.txt).
enum class RendererBackend
{
    Auto = 0,   // platform's preferred backend, falling back in priority order
    Vulkan,
    D3D11,
};

struct AppConfig
{
    int windowWidth = 1280;
    int windowHeight = 720;

    int renderWidth = 1920;
    int renderHeight = 1080;

    bool preservePresentationAspect = false;
    bool preferIntegerPresentationScale = true;

    // Preferred renderer. Ignored when the platform forces a single backend.
    // Overridden by "-renderer=<name>" on the command line or KES_RENDERER env var.
    RendererBackend renderer = RendererBackend::Auto;
};

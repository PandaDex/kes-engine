#include "engine.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include "command_line.h"
#include "engine_configuration.h"
#include "logger.h"
#include "pak_file_system.h"

#if PLATFORM_GDK
#include "platform/platform_gdk.h"
#elif PLATFORM_WINDOWS
#include "platform/platform_win.h"
#endif

#if PLATFORM_LINUX
#include "platform/platform_linux.h"
#endif

namespace
{
    // Tries each candidate backend in order (requested one first, then the
    // platform priority list). The window is recreated per attempt because
    // backends need different window flags (e.g. SDL_WINDOW_VULKAN).
    bool CreateWindowAndRenderer(EngineApp& app)
    {
        GameSettings& settings = *app.settings;

        // AppConfig::renderer (game code) wins over the player's saved preference.
        const RendererBackend preferred = app.config.renderer != RendererBackend::Auto
            ? app.config.renderer
            : settings.GetPreferredRenderer();

        const std::vector<RendererBackend> candidates = BuildRendererCandidateList(preferred);
        for (RendererBackend backend : candidates)
        {
            const char* backendName = RendererBackendName(backend);

            // Created windowed and hidden; ApplySettings switches to the saved
            // window mode before the window is shown.
            WindowDesc desc = {};
            desc.title = ENGINE_PROJECT_NAME;
            desc.width = settings.GetScreenResolution().x;
            desc.height = settings.GetScreenResolution().y;
            desc.mode = WindowMode::Windowed;
            desc.displayIndex = settings.GetDisplayIndex();
            desc.extraFlags = GetRendererWindowFlags(backend);
            if (!CreateAppWindow(app.window, desc))
                return false;
            EngineLogf("Application window created: \"%s\" (%dx%d, pixels %dx%d).", ENGINE_PROJECT_NAME,
                       app.window.width, app.window.height, app.window.pixelWidth, app.window.pixelHeight);

            AppConfig rendererConfig = app.config;
            rendererConfig.windowWidth = app.window.pixelWidth;
            rendererConfig.windowHeight = app.window.pixelHeight;
            rendererConfig.vsync = settings.IsVSyncEnabled();
            const IntPoint renderResolution = settings.GetRenderResolution();
            rendererConfig.renderWidth = renderResolution.x;
            rendererConfig.renderHeight = renderResolution.y;

            std::unique_ptr<IRenderer> renderer = CreateRenderer(backend);
            if (renderer && renderer->Init(app.window.handle, rendererConfig))
            {
                app.renderer = std::move(renderer);
                app.rendererSize = { app.window.pixelWidth, app.window.pixelHeight };
                EngineLogf("Renderer initialized: %s.", backendName);
                return true;
            }

            EngineLogf("Renderer \"%s\" failed to initialize.", backendName);
            renderer.reset();
            DestroyAppWindow(app.window);
        }

        EngineLog("No renderer backend could be initialized.");
        return false;
    }

    void ResizeRendererToWindow(EngineApp& app)
    {
        if (!app.renderer || !IsAppWindowRenderable(app.window))
            return;
        const IntPoint size = { app.window.pixelWidth, app.window.pixelHeight };
        if (size == app.rendererSize)
            return;
        if (app.renderer->Resize(size.x, size.y))
            app.rendererSize = size;
    }

    void ApplyInputMode(EngineApp& app)
    {
        switch (app.inputMode)
        {
        case InputMode::GameOnly:
            SetAppWindowCursor(app.window, false, true);
            app.input.gameInputEnabled = true;
            app.input.mouseLookEnabled = true;
            break;
        case InputMode::GameAndUI:
            SetAppWindowCursor(app.window, app.showMouseCursor, false);
            app.input.gameInputEnabled = true;
            app.input.mouseLookEnabled = false;
            break;
        case InputMode::UIOnly:
            SetAppWindowCursor(app.window, app.showMouseCursor, false);
            app.input.gameInputEnabled = false;
            app.input.mouseLookEnabled = false;
            break;
        }
    }

    // Sleeps until the next frame slot when a frame rate limit is set.
    void LimitFrameRate(EngineApp& app)
    {
        const float limit = app.settings ? app.settings->GetFrameRateLimit() : 0.0f;
        const unsigned long long now = SDL_GetPerformanceCounter();
        if (limit <= 0.0f)
        {
            app.frameDeadline = now;
            return;
        }

        const unsigned long long period = static_cast<unsigned long long>(app.performanceFrequency / limit);
        unsigned long long target = app.frameDeadline + period;
        // Fell more than a frame behind (hitch, breakpoint): don't try to catch up.
        if (now > target + period)
            target = now;

        if (now < target)
        {
            const double remainingSeconds = static_cast<double>(target - now) / app.performanceFrequency;
            SDL_DelayPrecise(static_cast<Uint64>(remainingSeconds * 1e9));
        }
        app.frameDeadline = target;
    }

    bool IsAltEnter(const SDL_Event& event)
    {
        return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
               (event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_KP_ENTER) &&
               (event.key.mod & SDL_KMOD_ALT) != 0;
    }
}

void EngineSetCommandLine(int argc, const char* const* argv)
{
    CommandLineInit(argc, argv);
}

bool EngineStart(EngineApp& app)
{
    const auto failStartup = [&app]() -> bool
    {
        EngineLog("Startup failed. Beginning shutdown cleanup.");
        EngineShutdown(app);
        return false;
    };

#if PLATFORM_GDK
    app.platform = new platform_gdk();
#elif PLATFORM_WINDOWS
    app.platform = new platform_win();
#elif PLATFORM_LINUX
    app.platform = new platform_linux();
#endif

    if (!LoggerInitialize(app.platform->get_executable_path()))
        std::printf("[Engine] Logger initialization failed. Continuing with console-only logging.\n");

    EngineLogf("Engine startup begin (platform: %s).", app.platform->get_name());

    if (!app.platform->initialize())
        return failStartup();
    app.platformInitialized = true;

    std::string pakError;
    if (!MountContentPakFiles(&pakError, app.platform->get_executable_path()))
    {
        EngineLogf("Pak mount failed: %s", pakError.c_str());
        return failStartup();
    }

    EngineLogf("Mounted %zu pak file(s) from %s.", GetMountedPakFileCount(), ENGINE_CONTENT_DIRECTORY);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        EngineLogf("SDL_Init failed: %s", SDL_GetError());
        return failStartup();
    }
    app.sdlInitialized = true;
    EngineLog("SDL subsystems initialized.");

    InputInit(app.input);
    EngineLog("Input initialized.");

    // Settings need SDL video (display queries) and come before the window.
    app.settings = app.settingsFactory ? app.settingsFactory() : std::make_unique<GameSettings>();
    app.settings->AttachEngine(nullptr, nullptr, &app.config);
    app.settings->SetActiveInstance(true);
    app.settings->LoadSettings();

    if (!CreateWindowAndRenderer(app))
        return failStartup();

    // Window mode, resolution, vsync and render resolution from the settings
    // (plus command line overrides), before the window becomes visible.
    app.settings->AttachEngine(&app.window, app.renderer.get(), &app.config);
    app.settings->ApplySettings(true);
    ResizeRendererToWindow(app);

    EngineLogf("Main camera initialized at position (%.2f, %.2f, %.2f).", app.camera.x, app.camera.y, app.camera.z);

    if (!app.world.Init(&app.camera, &app.input))
        return failStartup();
    app.worldInitialized = true;
    EngineLog("World initialized.");
    app.world.LoadMap(ENGINE_STARTUP_MAP);
    EngineLogf("Startup map load requested: %s.", ENGINE_STARTUP_MAP);
    app.performanceFrequency = static_cast<double>(SDL_GetPerformanceFrequency());
    app.prevCounter = SDL_GetPerformanceCounter();
    app.frameDeadline = app.prevCounter;
    app.running = true;
    ShowAppWindow(app.window);
    ApplyInputMode(app);
    EngineLog("Engine startup complete.");
    return true;
}

bool EngineTick(EngineApp& app)
{
    if (!app.running)
        return false;

    // Nothing to draw while minimized: block until an event arrives instead of
    // spinning the CPU (and the GPU on some drivers) at full speed.
    if (!IsAppWindowRenderable(app.window))
        SDL_WaitEventTimeout(nullptr, 100);

    InputBeginFrame(app.input);

    SDL_Event event = {};
    while (SDL_PollEvent(&event))
    {
        InputProcessEvent(app.input, event);

        const uint32_t windowEvents = AppWindowHandleEvent(app.window, event);
        if (windowEvents & AppWindowEvent_PixelSizeChanged)
            ResizeRendererToWindow(app);
        if ((windowEvents & AppWindowEvent_FocusChanged) && !app.window.focused)
            InputResetState(app.input);   // key-up events won't arrive while unfocused
        if ((windowEvents & AppWindowEvent_CloseRequested) || event.type == SDL_EVENT_QUIT)
            app.running = false;

        if (IsAltEnter(event))
            EngineToggleFullscreen(app);

#if ENGINE_ALLOW_DEBUG_MODE
        // F4: free / capture the mouse while developing.
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F4)
            EngineSetInputMode(app, app.inputMode == InputMode::GameOnly ? InputMode::GameAndUI : InputMode::GameOnly);
#endif
    }
    InputEndFrame(app.input);

    const Uint64 now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>((now - app.prevCounter) / app.performanceFrequency);
    app.prevCounter = now;
    if (dt > 0.25f)
        dt = 0.25f;

    RendererUpdateScene(app.sceneState, app.input);

    app.fixedAccumulator += dt;
    while (app.fixedAccumulator >= app.fixedDt)
    {
        app.world.FixedUpdate(app.fixedDt);
        app.fixedAccumulator -= app.fixedDt;
    }

    app.world.Update(dt);

    if (IsAppWindowRenderable(app.window))
    {
        app.renderer->RenderScene(app.sceneState, app.camera, app.world.GetScene(), app.world.GetAssets());
        app.renderer->BeginBackbufferOverlay();
        app.renderer->Present();
    }

    LimitFrameRate(app);
    return app.running;
}

void EngineRegisterEntityClass(EngineApp& app, const std::string& className, EntityFactoryFunction factory)
{
    app.world.RegisterEntityClass(className, std::move(factory));
}

void EngineSetPlayerClass(EngineApp& app, const std::string& className)
{
    app.world.SetPlayerClassName(className);
}

void EngineSetInputMode(EngineApp& app, InputMode mode)
{
    app.inputMode = mode;
    ApplyInputMode(app);
}

InputMode EngineGetInputMode(const EngineApp& app)
{
    return app.inputMode;
}

void EngineSetShowMouseCursor(EngineApp& app, bool show)
{
    app.showMouseCursor = show;
    ApplyInputMode(app);
}

void EngineSetWindowTitle(EngineApp& app, const char* title)
{
    SetWindowTitle(app.window, title);
}

void EngineToggleFullscreen(EngineApp& app)
{
    if (!app.settings)
        return;

    GameSettings& settings = *app.settings;
    const bool toFullscreen = settings.GetFullscreenMode() == WindowMode::Windowed;
    if (!toFullscreen && app.window.mode == WindowMode::Windowed)
        settings.SetScreenResolution(app.window.windowedSize);   // keep the size the player dragged to

    settings.SetFullscreenMode(toFullscreen ? settings.GetPreferredFullscreenMode() : WindowMode::Windowed);
    settings.ApplyResolutionSettings(false);
    settings.ConfirmVideoMode();
    settings.SaveSettings();
    ResizeRendererToWindow(app);
}

GameSettings& EngineGetGameSettings(EngineApp& app)
{
    return *app.settings;
}

void EngineShutdown(EngineApp& app)
{
    EngineLog("Engine shutdown begin.");
    app.running = false;
    InputShutdown(app.input);

    if (app.settings)
    {
        // Remember a window the player resized by dragging its border.
        if (app.renderer && app.window.handle &&
            app.settings->GetFullscreenMode() == WindowMode::Windowed && app.window.mode == WindowMode::Windowed)
        {
            app.settings->SetScreenResolution(app.window.windowedSize);
            app.settings->ConfirmVideoMode();
            app.settings->SaveSettings();
        }
        app.settings->AttachEngine(nullptr, nullptr, nullptr);
        app.settings->SetActiveInstance(false);
        app.settings.reset();
    }

    if (app.worldInitialized)
    {
        EngineLog("Shutting down world.");
        app.world.Shutdown();
        app.worldInitialized = false;
    }
    ClearMountedPakFiles();
    if (app.renderer)
    {
        app.renderer->Shutdown();
        app.renderer.reset();
        EngineLog("Renderer shut down.");
    }
    DestroyAppWindow(app.window);
    EngineLog("Window destroyed.");
    if (app.sdlInitialized)
    {
        SDL_Quit();
        app.sdlInitialized = false;
        EngineLog("SDL shut down.");
    }
    if (app.platform)
    {
        if (app.platformInitialized)
        {
            app.platform->shutdown();
            app.platformInitialized = false;
        }
        delete app.platform;
        app.platform = nullptr;
    }
    EngineLog("Engine shutdown complete.");
    LoggerShutdown();
}

#include "engine.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include "engine_configuration.h"
#include "logger.h"
#include "pak_file_system.h"

#if PLATFORM_WINDOWS
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
        const std::vector<RendererBackend> candidates = BuildRendererCandidateList(app.config.renderer);
        for (RendererBackend backend : candidates)
        {
            const char* backendName = RendererBackendName(backend);
            if (!CreateAppWindow(app.window, app.config, ENGINE_PROJECT_NAME, GetRendererWindowFlags(backend)))
                return false;
            EngineLogf("Application window created: \"%s\" (%dx%d).", ENGINE_PROJECT_NAME, app.window.width, app.window.height);

            std::unique_ptr<IRenderer> renderer = CreateRenderer(backend);
            if (renderer && renderer->Init(app.window.handle, app.config))
            {
                app.renderer = std::move(renderer);
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
}

bool EngineStart(EngineApp& app)
{
    const auto failStartup = [&app]() -> bool
    {
        EngineLog("Startup failed. Beginning shutdown cleanup.");
        EngineShutdown(app);
        return false;
    };

#if PLATFORM_WINDOWS
    app.platform = new platform_win();
#endif

#if PLATFORM_LINUX
    app.platform = new platform_linux();
#endif

    if (!LoggerInitialize(app.platform->get_executable_path()))
        std::printf("[Engine] Logger initialization failed. Continuing with console-only logging.\n");

    EngineLog("Engine startup begin.");
    EngineLogf(
        "Startup configuration: window=%dx%d render=%dx%d preserveAspect=%s integerScale=%s.",
        app.config.windowWidth,
        app.config.windowHeight,
        app.config.renderWidth,
        app.config.renderHeight,
        app.config.preservePresentationAspect ? "true" : "false",
        app.config.preferIntegerPresentationScale ? "true" : "false");

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
        return false;
    }
    app.sdlInitialized = true;
    EngineLog("SDL subsystems initialized.");

    InputInit(app.input);
    EngineLog("Input initialized.");

    if (!CreateWindowAndRenderer(app))
        return failStartup();

    EngineLogf("Main camera initialized at position (%.2f, %.2f, %.2f).", app.camera.x, app.camera.y, app.camera.z);

    if (!app.world.Init(&app.camera, &app.input))
        return failStartup();
    app.worldInitialized = true;
    EngineLog("World initialized.");
    app.world.LoadMap(ENGINE_STARTUP_MAP);
    EngineLogf("Startup map load requested: %s.", ENGINE_STARTUP_MAP);
    app.performanceFrequency = static_cast<double>(SDL_GetPerformanceFrequency());
    app.prevCounter = SDL_GetPerformanceCounter();
    app.running = true;
    ShowAppWindow(app.window);
    EngineLog("Engine startup complete.");
    return true;
}

bool EngineTick(EngineApp& app)
{
    if (!app.running)
        return false;

    InputBeginFrame(app.input);

    SDL_Event event = {};
    while (SDL_PollEvent(&event))
    {
        InputProcessEvent(app.input, event);

        if (event.window.type == SDL_EVENT_WINDOW_RESIZED)
        {
            SetWindowSize(app.window, event.window.data1, event.window.data2);
            app.renderer->Resize(app.window.width, app.window.height);
        }

        if (event.type == SDL_EVENT_QUIT){
        app.running = false;
        }
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

    app.renderer->RenderScene(app.sceneState, app.camera, app.world.GetScene(), app.world.GetAssets());
    app.renderer->BeginBackbufferOverlay();
    app.renderer->Present();

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

void EngineShutdown(EngineApp& app)
{
    EngineLog("Engine shutdown begin.");
    app.running = false;
    InputShutdown(app.input);

    if (app.worldInitialized)
    {
        EngineLog("Shutting down world.");
        app.world.Shutdown();
        app.worldInitialized = false;
    }
    ClearMountedPakFiles();
    EngineLog("Debug console shut down.");
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
    EngineLog("Engine shutdown complete.");
    LoggerShutdown();
}

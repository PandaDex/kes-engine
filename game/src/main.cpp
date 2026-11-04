#include <utility>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>
#include <RmlUi/Core.h>
#include "engine.h"
#include "engine_configuration.h"
#include "file_paths.h"

// Game entry point. Hook up things like the Steam API or Discord RPC here,
// not in the engine.

namespace
{
    // Example UI (content/ui): a HUD bound to a data model and a pause menu
    // whose buttons call C++ actions. See docs/ui-system.md.
    struct ExampleUi
    {
        Rml::ElementDocument* hud = nullptr;
        Rml::ElementDocument* pauseMenu = nullptr;
        Rml::DataModelHandle hudModel;
        int fps = 0;
        int frames = 0;
        Uint64 fpsWindowStart = 0;
    };

    void SetPaused(EngineApp& app, ExampleUi& ui, bool paused)
    {
        if (!ui.pauseMenu)
            return;
        if (paused)
            ui.pauseMenu->Show(Rml::ModalFlag::Modal);
        else
            ui.pauseMenu->Hide();
        // Stops entities and audio; UIOnly gives the menu the mouse, keyboard
        // and gamepad (UI_* actions).
        EngineSetPaused(app, paused);
        EngineSetInputMode(app, paused ? InputMode::UIOnly : InputMode::GameOnly);
    }

    void InitExampleUi(EngineApp& app, ExampleUi& example)
    {
        UiSystem& ui = EngineGetUi(app);
        if (!ui.IsInitialized())
            return;

        // Data models must exist before documents that use them are loaded.
        if (Rml::DataModelConstructor model = ui.GetContext()->CreateDataModel("hud"))
        {
            model.Bind("fps", &example.fps);
            example.hudModel = model.GetModelHandle();
        }
        example.hud = ui.LoadDocument("ui/hud.rml");
        example.pauseMenu = ui.LoadDocument("ui/pause_menu.rml", false);

        ui.RegisterAction("resume", [&app, &example](Rml::Event&, const std::string&) { SetPaused(app, example, false); });
        ui.RegisterAction("toggle_fullscreen", [&app](Rml::Event&, const std::string&) { EngineToggleFullscreen(app); });
        ui.RegisterAction("quit", [&app](Rml::Event&, const std::string&) { app.running = false; });
        example.fpsWindowStart = SDL_GetPerformanceCounter();
    }

    void UpdateExampleUi(EngineApp& app, ExampleUi& example)
    {
        // InputAction_Pause (Esc / Start, config/default.imap) opens the menu.
        // While it is open, game actions read as idle and the UI reports
        // UI_Back (Esc, B, Start) instead.
        const bool paused = example.pauseMenu && example.pauseMenu->IsVisible();
        if ((!paused && InputActionPressed(app.input, "InputAction_Pause")) ||
            (paused && EngineGetUi(app).WasBackPressed()))
            SetPaused(app, example, !paused);

#if ENGINE_ALLOW_DEBUG_MODE
        // F5 / F6: a sound to the right / left of the camera, to check stereo.
        for (const auto& [key, side] : { std::pair{ SDL_SCANCODE_F5, 4.0f }, std::pair{ SDL_SCANCODE_F6, -4.0f } })
        {
            if (InputKeyPressed(app.input, key))
            {
                const Vector3 position = CameraGetPosition(app.camera) + CameraGetRight(app.camera) * side;
                app.world.GetAudio().Play3D(position, 0.05f, "sounds/default.ksound");
            }
        }
#endif

        ++example.frames;
        const Uint64 now = SDL_GetPerformanceCounter();
        const double elapsed = static_cast<double>(now - example.fpsWindowStart) / static_cast<double>(SDL_GetPerformanceFrequency());
        if (elapsed >= 0.5)
        {
            example.fps = static_cast<int>(example.frames / elapsed + 0.5);
            example.frames = 0;
            example.fpsWindowStart = now;
            if (example.hudModel)
                example.hudModel.DirtyVariable("fps");
        }
    }
}

int main(int argc, char* argv[])
{
    CollectPositionalCommandLineArguments(argc, argv);
    // Stores the command line for the engine: -renderer=vulkan|d3d11,
    // -windowed / -fullscreen / -borderless, -ResX= -ResY=, -vsync / -novsync, ...
    EngineSetCommandLine(argc, argv);

    EngineApp app = {};

    // Preferred renderer for this game (Auto = platform default)
    // app.config.renderer = RendererBackend::D3D11;

    // Override the default player class:
    // EngineRegisterPlayerClass<CLASS_NAME>(app, "CLASS_NAME");
    if (!EngineStart(app))
        return 1;

    ExampleUi exampleUi;
    InitExampleUi(app, exampleUi);

    while (EngineTick(app))
    {
        UpdateExampleUi(app, exampleUi);
    }

    EngineShutdown(app);
    return app.exitCode;
}

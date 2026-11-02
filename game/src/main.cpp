#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>
#include <RmlUi/Core.h>
#include "engine.h"
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
        // InputAction_Pause (config/default.imap) opens the menu; while it is
        // open, game actions read as idle, so Escape is checked directly.
        const bool paused = example.pauseMenu && example.pauseMenu->IsVisible();
        if ((!paused && InputActionPressed(app.input, "InputAction_Pause")) ||
            (paused && InputKeyPressed(app.input, SDL_SCANCODE_ESCAPE)))
            SetPaused(app, example, !paused);

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
    {
        EngineShutdown(app);
        return 1;
    }

    ExampleUi exampleUi;
    InitExampleUi(app, exampleUi);

    while (EngineTick(app))
    {
        UpdateExampleUi(app, exampleUi);
    }

    EngineShutdown(app);
    return 0;
}

#include <SDL3/SDL_main.h>
#include "engine.h"
#include "file_paths.h"

// Game entry point. Hook up things like the Steam API or Discord RPC here,
// not in the engine.

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

    while (EngineTick(app))
    {
    }

    EngineShutdown(app);
    return 0;
}

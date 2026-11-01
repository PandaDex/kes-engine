#include <SDL3/SDL_main.h>
#include "engine.h"
#include "file_paths.h"

//This file is the start of everything, engine initialization etc.
//You should hook up things like steam API or discord's RPC here instead of engine.

int main(int argc, char* argv[])
{
    CollectPositionalCommandLineArguments(argc, argv);
    // Lets players pick a backend with "-renderer=vulkan" / "-renderer=d3d11"
    // (ignored on platforms that force a single renderer).
    ApplyRendererCommandLine(argc, argv);

    //Initialize engine
    EngineApp app = {};

    //Preferred renderer for this game (Auto = platform default)
    // app.config.renderer = RendererBackend::D3D11;

    //You can override a default player class like that
    // EngineRegisterPlayerClass<CLASS_NAME>(app, "CLASS_NAME");
    if (!EngineStart(app))
    {
        EngineShutdown(app);
        return 1;
    }

    while (EngineTick(app))
    {
        //Main loop
    }

    //shutdown
    EngineShutdown(app);
    return 0;
}

#include "platform_gdk.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <XGameRuntimeInit.h>
#include <XGameErr.h>
#include <SDL3/SDL_messagebox.h>
#include "../logger.h"

bool platform_gdk::initialize()
{
    const HRESULT hr = XGameRuntimeInitialize();
    if (FAILED(hr))
    {
        const char* reason = "Failed to initialize the Microsoft Gaming Runtime.";
        if (hr == E_GAMERUNTIME_DLL_NOT_FOUND)
            reason = "The Microsoft Gaming Services are not installed. Install \"Gaming Services\" from the Microsoft Store and try again.";
        else if (hr == E_GAMERUNTIME_VERSION_MISMATCH)
            reason = "The Microsoft Gaming Services are out of date. Update \"Gaming Services\" from the Microsoft Store and try again.";
        else if (hr == E_GAMEPACKAGE_CONFIG_NO_ROOT_NODE || hr == E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER)
            reason = "MicrosoftGame.config is missing or invalid next to the executable.";

        EngineLogf("XGameRuntimeInitialize failed: 0x%08X. %s", static_cast<unsigned>(hr), reason);
        // SDL is not initialized yet; message boxes work without it.
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Startup error", reason, nullptr);
        return false;
    }

    m_runtimeInitialized = true;
    EngineLog("Gaming Runtime initialized.");
    return true;
}

void platform_gdk::shutdown()
{
    if (!m_runtimeInitialized)
        return;

    XGameRuntimeUninitialize();
    m_runtimeInitialized = false;
    EngineLog("Gaming Runtime shut down.");
}

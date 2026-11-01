#pragma once
#include "platform_win.h"

// Microsoft GDK for PC (KES_PLATFORM=GDK).
//
// Desktop GDK titles are regular Win32 programs that additionally use the
// Gaming Runtime (XGameRuntime: XUser, XStore, XGameSave, ...). The runtime
// must be initialized before any X* API is used and requires a
// MicrosoftGame.config next to the executable.
class platform_gdk : public platform_win
{
public:
    bool initialize() override;
    void shutdown() override;
    const char* get_name() const override { return "GDK"; }

private:
    bool m_runtimeInitialized = false;
};

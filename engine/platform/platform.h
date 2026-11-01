#pragma once

#include "../engine_types.h"

class Platform
{
public:
    virtual ~Platform() = default;

    // Called first thing in EngineStart / last thing in EngineShutdown.
    // Platforms with runtime services (e.g. GDK's Gaming Runtime) set them up here.
    virtual bool initialize()
    {
        return true;
    }

    virtual void shutdown()
    {
    }

    virtual const char* get_name() const
    {
        return "unknown";
    }

    virtual Vector2 get_display_resolution(int displayIndex)
    {
        return Vector2(0,0);
    }

    virtual String get_display_name(int displayIndex)
    {
        return String("");
    }

    virtual String get_executable_path()
    {
        return String("");
    }
};

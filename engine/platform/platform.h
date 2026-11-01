#pragma once

#include "../engine_types.h"

class Platform
{
public:
    virtual ~Platform() = default;

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

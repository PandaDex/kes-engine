#pragma once
#include "platform.h"

class platform_linux : public Platform
{
public:
    virtual Vector2 get_display_resolution(int displayIndex) override;
    virtual String get_executable_path() override;
    virtual const char* get_name() const override { return "Linux"; }
};

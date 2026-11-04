#include "test_framework.h"
#include "command_line.h"

TEST_CASE("command line: reads flags and values")
{
    const char* argv[] = { "game", "-Windowed", "--ResX=1280", "-renderer", "d3d11", "-Scale=0.5" };
    CommandLineInit(6, argv);

    CHECK(CommandLineHasFlag("windowed"));
    CHECK(!CommandLineHasFlag("fullscreen"));

    int width = 0;
    CHECK(CommandLineGetInt("resx", width));
    CHECK(width == 1280);

    std::string renderer;
    CHECK(CommandLineGetValue("renderer", renderer));
    CHECK(renderer == "d3d11");

    float scale = 0.0f;
    CHECK(CommandLineGetFloat("scale", scale));
    CHECK_NEAR(scale, 0.5, 1e-6);

    std::string missing;
    CHECK(!CommandLineGetValue("missing", missing));

    CommandLineInit(0, nullptr);
}

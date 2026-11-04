#include "test_framework.h"
#include "ini_file.h"

TEST_CASE("ini: parses sections, ignoring case and comments")
{
    IniFile ini;
    ini.LoadFromString("; comment\n[Video]\nWidth=1280\nScale=0.5\nVSync=True\nName=kes\n");

    CHECK(ini.Has("video", "WIDTH"));
    CHECK(ini.GetInt("Video", "Width", 0) == 1280);
    CHECK_NEAR(ini.GetFloat("Video", "Scale", 0.0f), 0.5, 1e-6);
    CHECK(ini.GetBool("Video", "VSync", false));
    CHECK(ini.GetString("Video", "Name") == "kes");
}

TEST_CASE("ini: missing keys return the fallback")
{
    IniFile ini;
    ini.LoadFromString("[A]\nKey=1\n");

    CHECK(!ini.Has("A", "Other"));
    CHECK(ini.Find("B", "Key") == nullptr);
    CHECK(ini.GetInt("B", "Key", 7) == 7);
    CHECK(ini.GetString("A", "Other", "x") == "x");
}

TEST_CASE("ini: survives a text round trip")
{
    IniFile ini;
    ini.SetInt("Video", "Width", 640);
    ini.SetBool("Video", "VSync", true);
    ini.SetString("Audio", "Device", "default");

    IniFile copy;
    copy.LoadFromString(ini.ToString());

    CHECK(copy.GetInt("Video", "Width", 0) == 640);
    CHECK(copy.GetBool("Video", "VSync", false));
    CHECK(copy.GetString("Audio", "Device") == "default");
}

TEST_CASE("ini: survives a file round trip")
{
    kestest::TempDir dir;
    const std::string path = dir.File("settings.ini");

    IniFile ini;
    ini.SetInt("Video", "Width", 640);
    REQUIRE(ini.SaveToFile(path));

    IniFile copy;
    REQUIRE(copy.LoadFromFile(path));
    CHECK(copy.GetInt("Video", "Width", 0) == 640);
}

TEST_CASE("ini: merge overlays keys and remove deletes them")
{
    IniFile base;
    base.LoadFromString("[A]\nKeep=1\nReplace=1\n");
    IniFile overlay;
    overlay.LoadFromString("[A]\nReplace=2\n[B]\nNew=3\n");

    base.Merge(overlay);
    CHECK(base.GetInt("A", "Keep", 0) == 1);
    CHECK(base.GetInt("A", "Replace", 0) == 2);
    CHECK(base.GetInt("B", "New", 0) == 3);

    base.RemoveKey("A", "Keep");
    CHECK(!base.Has("A", "Keep"));
}

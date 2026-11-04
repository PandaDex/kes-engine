#include "test_framework.h"
#include <filesystem>
#include "material_file.h"
#include "text_parse_utils.h"

#if PLATFORM_WINDOWS
#include <Windows.h>
#endif

TEST_CASE("text: Quote and Unquote round trip")
{
    for (const char* value : { "plain", "textures\\rock.ktex", "say \"hi\"", "ends with \\" })
        CHECK(textparse::Unquote(textparse::Quote(value)) == value);
}

TEST_CASE("material: Transparent = false beats a translucent base color")
{
    kestest::TempDir dir;
    const std::string path = dir.File("glass.mat");
    const std::string text = "\xEF\xBB\xBFMaterial {\n    BaseColor = \"1,1,1,0.5\"\n    Transparent = false\n}\n";
    kestest::WriteBytes(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());

    MaterialFileData file;
    REQUIRE(LoadMaterialFile(path.c_str(), file));
    REQUIRE(EvaluateMaterialGraph(file));
    CHECK((file.material.flags & MaterialFlag_Transparent) == 0);

    MaterialFileData implied;
    implied.material.baseColor[3] = 0.5f;
    REQUIRE(EvaluateMaterialGraph(implied));
    CHECK((implied.material.flags & MaterialFlag_Transparent) != 0);
}

#if PLATFORM_WINDOWS
TEST_CASE("paths: any folder name converts to a narrow string")
{
    CHECK(GetACP() == CP_UTF8);   // from cmake/kes_utf8.manifest
    const std::filesystem::path path(L"C:\\Users\\\u3042\u0141\\AppData");
    const std::string narrow = path.string();
    CHECK(std::filesystem::path(narrow) == path);
}
#endif

#include "test_framework.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include "file_paths.h"
#include "localization.h"
#include "pak_file_system.h"

namespace
{
    const char *const kEnglish =
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Language: en\\n\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\n"
        "msgid \"MainMenu.NewGame\"\n"
        "msgstr \"New Game\"\n"
        "\n"
        "msgid \"MainMenu.Credits\"\n"
        "msgstr \"Credits\"\n"
        "\n"
        "msgid \"MainMenu.Quit\"\n"
        "msgstr \"Quit\"\n"
        "\n"
        "msgid \"Inventory.Apples\"\n"
        "msgid_plural \"Inventory.Apples\"\n"
        "msgstr[0] \"{0} apple\"\n"
        "msgstr[1] \"{0} apples\"\n";

    const char *const kPolish =
        "# Polish translation\n"
        "msgid \"\"\n"
        "msgstr \"\"\n"
        "\"Language: pl\\n\"\n"
        "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
        "\"Plural-Forms: nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);\\n\"\n"
        "\n"
        "#: ui/menu.rml:4\n"
        "msgid \"MainMenu.NewGame\"\n"
        "msgstr \"Nowa gra\"\n"
        "\n"
        "msgid \"#Door.Open\"\n"
        "msgstr \"Otwórz\"\n"
        "\n"
        "msgid \"Inventory.Apples\"\n"
        "msgid_plural \"Inventory.Apples\"\n"
        "msgstr[0] \"{0} jabłko\"\n"
        "msgstr[1] \"{0} jabłka\"\n"
        "msgstr[2] \"{0} jabłek\"\n"
        "\n"
        "msgid \"Intro.Text\"\n"
        "msgstr \"\"\n"
        "\"Linia pierwsza\\n\"\n"
        "\"Linia \\\"druga\\\"\"\n"
        "\n"
        "#, fuzzy\n"
        "msgid \"MainMenu.Options\"\n"
        "msgstr \"Opcje?\"\n"
        "\n"
        "msgid \"MainMenu.Quit\"\n"
        "msgstr \"\"\n"
        "\n"
        "msgctxt \"menu\"\n"
        "msgid \"With.Context\"\n"
        "msgstr \"Z kontekstem\"\n"
        "\n"
        "#~ msgid \"Old.Thing\"\n"
        "#~ msgstr \"Stare\"\n";

    struct AssetRootScope
    {
        explicit AssetRootScope(const std::filesystem::path &root) : previous(GetAssetRootOverride())
        {
            SetAssetRootOverride(root.string().c_str());
        }
        ~AssetRootScope()
        {
            SetAssetRootOverride(previous.c_str());
            LocalizationShutdown();
        }
        std::string previous;
    };

    void WriteText(const std::filesystem::path &path, const std::string &text)
    {
        std::filesystem::create_directories(path.parent_path());
        kestest::WriteBytes(path.string(), reinterpret_cast<const uint8_t *>(text.data()), text.size());
    }
}

TEST_CASE("po: entries are keyed by msgid")
{
    PoCatalog catalog;
    std::string error;
    REQUIRE(catalog.Parse(kPolish, &error));
    CHECK(error.empty());
    CHECK(catalog.GetLanguage() == "pl");
    CHECK(catalog.GetPluralCount() == 3);

    REQUIRE(catalog.Find("MainMenu.NewGame"));
    CHECK(*catalog.Find("MainMenu.NewGame") == "Nowa gra");
    CHECK(*catalog.Find("#MainMenu.NewGame") == "Nowa gra");
    CHECK(*catalog.Find("Door.Open") == "Otwórz");
    CHECK(*catalog.Find("Intro.Text") == "Linia pierwsza\nLinia \"druga\"");
    CHECK(catalog.Find("New Game") == nullptr);

    CHECK(*catalog.FindPlural("Inventory.Apples", 1) == "{0} jabłko");
    CHECK(*catalog.FindPlural("Inventory.Apples", 3) == "{0} jabłka");
    CHECK(*catalog.FindPlural("Inventory.Apples", 5) == "{0} jabłek");
    CHECK(*catalog.FindPlural("Inventory.Apples", 22) == "{0} jabłka");
    CHECK(*catalog.FindPlural("Inventory.Apples", 12) == "{0} jabłek");
}

TEST_CASE("po: fuzzy, untranslated, msgctxt and obsolete entries")
{
    PoCatalog catalog;
    REQUIRE(catalog.Parse(kPolish));
    CHECK(catalog.Find("MainMenu.Options") == nullptr);
    CHECK(catalog.FindEntry("MainMenu.Options") != nullptr);
    CHECK(catalog.Find("MainMenu.Quit") == nullptr);
    CHECK(catalog.FindEntry("With.Context") == nullptr);
    CHECK(catalog.FindEntry("Old.Thing") == nullptr);
    CHECK(catalog.GetSkippedCount() == 1);
    CHECK(catalog.GetEntryCount() == 6);
}

TEST_CASE("po: escapes and a BOM")
{
    PoCatalog catalog;
    REQUIRE(catalog.Parse("\xEF\xBB\xBFmsgid \"K\"\nmsgstr \"\\x41\\101\\\\\\t\"\n"));
    REQUIRE(catalog.Find("K"));
    CHECK(*catalog.Find("K") == "AA\\\t");
}

TEST_CASE("po: reports malformed files with a line number")
{
    PoCatalog catalog;
    std::string error;
    CHECK(!catalog.Parse("msgid \"\"\nmsgstr \"\"\nmsgid \"K\"\nmsgstr \"b\n", &error));
    CHECK(error.find("line 4") != std::string::npos);

    CHECK(!catalog.Parse("msgid \"a\"\nmsgid \"b\"\nmsgstr \"c\"\n", &error));
    CHECK(!catalog.Parse("msgstr \"c\"\n", &error));
    CHECK(!catalog.Parse("msgid \"K\"\nmsgstr[1] \"c\"\n", &error));
    CHECK(!catalog.Parse("msgid \"K\"\nbogus \"c\"\n", &error));
    CHECK(!catalog.Parse("msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=ISO-8859-2\\n\"\n", &error));
    CHECK(error.find("UTF-8") != std::string::npos);
    CHECK(catalog.GetEntryCount() == 0);
}

TEST_CASE("po: plural expressions")
{
    size_t count = 0;
    std::vector<PoCatalog::PluralNode> nodes;
    CHECK(PoCatalog::ParsePluralForms("nplurals=1; plural=0;", count, nodes));
    CHECK(count == 1);
    CHECK(PoCatalog::ParsePluralForms("nplurals=2; plural=n > 1;", count, nodes));
    CHECK(!PoCatalog::ParsePluralForms("nplurals=2; plural=n >;", count, nodes));
    CHECK(!PoCatalog::ParsePluralForms("nplurals=2; plural=(n == 1;", count, nodes));
    CHECK(!PoCatalog::ParsePluralForms("plural=n != 1;", count, nodes));

    // Arabic: 6 forms, exercises nested ternaries and modulo.
    PoCatalog arabic;
    REQUIRE(arabic.Parse("msgid \"\"\nmsgstr \"Plural-Forms: nplurals=6; plural=n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : "
                         "n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5;\\n\"\n"));
    CHECK(arabic.PluralIndex(0) == 0);
    CHECK(arabic.PluralIndex(1) == 1);
    CHECK(arabic.PluralIndex(2) == 2);
    CHECK(arabic.PluralIndex(105) == 3);
    CHECK(arabic.PluralIndex(111) == 4);
    CHECK(arabic.PluralIndex(100) == 5);

    PoCatalog english;
    CHECK(english.PluralIndex(1) == 0);
    CHECK(english.PluralIndex(0) == 1);
}

TEST_CASE("localization: keys, language codes and LocFormat")
{
    CHECK(LocalizationIsKeyReference("#MainMenu.NewGame"));
    CHECK(LocalizationIsKeyReference("#HUD.Ammo_Count-2"));
    CHECK(!LocalizationIsKeyReference("MainMenu.NewGame"));
    CHECK(!LocalizationIsKeyReference("#1 Player"));
    CHECK(!LocalizationIsKeyReference("#Main Menu"));
    CHECK(!LocalizationIsKeyReference("#"));
    CHECK(LocalizationStripKey(" #A.B ") == "A.B");

    CHECK(LocalizationNormalizeLanguageCode("pt-br") == "pt_BR");
    CHECK(LocalizationNormalizeLanguageCode("PL") == "pl");
    CHECK(LocalizationNormalizeLanguageCode("pl_PL.UTF-8") == "pl_PL");
    CHECK(LocalizationNormalizeLanguageCode("sr_RS@latin") == "sr_RS");
    CHECK(LocalizationNormalizeLanguageCode("../x").empty());

    CHECK(LocFormat("{1} of {0}", {"a", "b"}) == "b of a");
    CHECK(LocFormat("{2} {{x}} {", {"a"}) == "{2} {{x}} {");
}

TEST_CASE("localization: loads content/localization/<code>.po and falls back")
{
    kestest::TempDir dir;
    WriteText(dir.Path("localization") / "en.po", kEnglish);
    WriteText(dir.Path("localization") / "pl.po", kPolish);
    WriteText(dir.Path("localization") / "pt_BR.po", "msgid \"MainMenu.NewGame\"\nmsgstr \"Novo jogo\"\n");
    AssetRootScope scope(dir.Root());

    const std::vector<std::string> languages = LocalizationGetAvailableLanguages();
    REQUIRE(languages.size() == 3);
    CHECK(languages[0] == "en");
    CHECK(languages[1] == "pl");
    CHECK(languages[2] == "pt_BR");

    const uint64_t generation = LocalizationGetGeneration();
    CHECK(LocalizationSetLanguage("pl_PL"));   // region falls back to the base language
    CHECK(LocalizationGetLanguage() == "pl");
    CHECK(LocalizationGetGeneration() != generation);
    CHECK(Tr("#MainMenu.NewGame") == "Nowa gra");
    CHECK(Tr("MainMenu.NewGame") == "Nowa gra");
    CHECK(Tr("#MainMenu.Credits") == "Credits");   // not in pl.po
    CHECK(Tr("#MainMenu.Quit") == "Quit");         // empty in pl.po
    CHECK(Tr("#MainMenu.Options") == "#MainMenu.Options");   // fuzzy in pl.po, not in en.po
    CHECK(Tr("#Missing.Key") == "#Missing.Key");
    CHECK(!LocalizationHasKey("#Missing.Key"));
    CHECK(LocalizationHasKey("#MainMenu.Credits"));
    CHECK(LocFormat(TrPlural("#Inventory.Apples", 5), {"5"}) == "5 jabłek");

    CHECK(LocalizationSetLanguage("pt-BR"));
    CHECK(Tr("#MainMenu.NewGame") == "Novo jogo");
    CHECK(TrPlural("#Inventory.Apples", 1) == "{0} apple");
    CHECK(TrPlural("#Inventory.Apples", 2) == "{0} apples");

    CHECK(LocalizationSetLanguage("en"));
    CHECK(LocalizationGetLanguage() == "en");
    CHECK(Tr("#MainMenu.NewGame") == "New Game");

    CHECK(!LocalizationSetLanguage("de"));
    CHECK(LocalizationGetLanguage() == "en");
    CHECK(LocalizationGetRequestedLanguage() == "de");
    CHECK(Tr("#MainMenu.Credits") == "Credits");
}

TEST_CASE("localization: a broken .po file leaves the source language")
{
    kestest::TempDir dir;
    WriteText(dir.Path("localization") / "en.po", kEnglish);
    WriteText(dir.Path("localization") / "pl.po", "msgid \"MainMenu.NewGame\"\n");
    AssetRootScope scope(dir.Root());

    CHECK(!LocalizationSetLanguage("pl"));
    CHECK(LocalizationGetLanguage() == "en");
    CHECK(Tr("#MainMenu.NewGame") == "New Game");
}

TEST_CASE("localization: languages are listed from a mounted pak")
{
    kestest::TempDir source;
    WriteText(source.Path("content") / "localization" / "DE.po", "msgid \"MainMenu.NewGame\"\nmsgstr \"Neues Spiel\"\n");
    kestest::TempDir pak;
    const std::string pakPath = pak.File("game.pak");
    REQUIRE(CreatePakFile(source.Path("content").string().c_str(), pakPath.c_str(), nullptr));

    kestest::TempDir emptyRoot;
    AssetRootScope scope(emptyRoot.Root());
    REQUIRE(MountPakFile(pakPath.c_str()));

    const std::vector<std::string> files = ListAssetFiles(kLocalizationDirectory, ".po");
    REQUIRE(files.size() == 1);
    CHECK(files[0] == "de.po");
    CHECK(LocalizationSetLanguage("de"));
    CHECK(Tr("#MainMenu.NewGame") == "Neues Spiel");
    ClearMountedPakFiles();
}

TEST_CASE("localization: the game's .po files parse")
{
    const std::string roots = KES_TEST_CONTENT_DIRS;
    const std::filesystem::path directory = std::filesystem::path(roots.substr(0, roots.find('|'))) / kLocalizationDirectory;
    REQUIRE(std::filesystem::is_directory(directory));
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().extension() != ".po")
            continue;
        const std::vector<uint8_t> bytes = kestest::ReadBytes(entry.path().string());
        PoCatalog catalog;
        std::string error;
        const bool parsed = catalog.Parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), &error);
        if (!parsed)
            std::printf("%s: %s\n", entry.path().string().c_str(), error.c_str());
        CHECK(parsed);
        CHECK(catalog.GetSkippedCount() == 0);
    }
}

#include "test_framework.h"
#include <RmlUi/Core.h>
#include "engine_configuration.h"
#include <filesystem>
#include "file_paths.h"
#include "input.h"
#include "localization.h"
#include "on_screen_debug.h"
#include "render_backend.h"
#include "ui_system.h"

// Lays out the real document with RmlUi and SDL's offscreen video driver; no
// GPU or visible window.
namespace
{
    class NullRenderer final : public IRenderer
    {
    public:
        RendererBackend GetBackend() const override { return RendererBackend::Auto; }
        bool Init(SDL_Window*, const AppConfig&) override { return true; }
        void Shutdown() override {}
        bool Resize(int, int) override { return true; }
        bool SetRenderResolution(int, int) override { return true; }
        void SetVSync(bool) override {}
        bool IsVSyncEnabled() const override { return false; }
        void SetGraphicsOptions(const GraphicsOptions&) override {}
        GraphicsOptions GetGraphicsOptions() const override { return {}; }
        void RenderScene(const SceneState&, const Camera&, const Scene&, const AssetSystem&) override {}
        void BeginBackbufferOverlay() override {}
        UiTextureHandle CreateUiTexture(int, int, const uint8_t*) override { return ++next; }
        void DestroyUiTexture(UiTextureHandle) override {}
        void RenderUi(const UiDrawList&) override {}
        void Present() override {}
        const std::string& GetFatalError() const override { return error; }
        UiTextureHandle next = 0;
        std::string error;
    };

    struct OffscreenUi
    {
        SDL_Window* window = nullptr;
        NullRenderer renderer;
        UiSystem ui;

        explicit OffscreenUi(InputState* input = nullptr)
        {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
            REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
            window = SDL_CreateWindow("ui test", 1280, 720, SDL_WINDOW_HIDDEN);
            REQUIRE(window != nullptr);
            const std::string roots = KES_TEST_CONTENT_DIRS;
            SetAssetRootOverride(roots.substr(roots.find('|') + 1).c_str());   // engine/assets: fonts, kes.rcss
            REQUIRE(ui.Init(window, &renderer, 1280, 720, input));
        }

        ~OffscreenUi()
        {
            OnScreenDebugShutdown(ui);
            ui.Shutdown();
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
            SetAssetRootOverride("");
        }

        void Frame(float dt = 0.016f)
        {
            OnScreenDebugUpdate(ui, dt);
            ui.Update();
        }

        // The message boxes, top to bottom.
        std::vector<Rml::Element*> Messages()
        {
            std::vector<Rml::Element*> result;
            if (Rml::ElementDocument* document = ui.GetContext()->GetDocument("kes-on-screen-debug"))
            {
                for (int i = 0; i < document->GetNumChildren(); ++i)
                    result.push_back(document->GetChild(i)->GetChild(0));
            }
            return result;
        }
    };

    Rml::Vector2f BorderSize(Rml::Element* element)
    {
        return element->GetBox().GetSize(Rml::BoxArea::Border);
    }
}

#if ENGINE_ALLOW_DEBUG_MODE
TEST_CASE("on-screen debug: boxes are as wide as their text, newest on top")
{
    OffscreenUi ui;
    AddOnScreenDebugMessage(-1, 5.0f, DebugMessageType::Info, "Info here");
    AddOnScreenDebugMessage(-1, 5.0f, DebugMessageType::Error, "A much longer error message, wider than the info one");
    ui.Frame();

    const std::vector<Rml::Element*> messages = ui.Messages();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0]->IsClassSet("error"));
    CHECK(messages[1]->IsClassSet("info"));
    const Rml::Vector2f error = BorderSize(messages[0]);
    const Rml::Vector2f info = BorderSize(messages[1]);
    CHECK(info.x > 40.0f);
    CHECK(error.x > info.x * 2.0f);
    CHECK(error.x < 1280.0f * 0.8f);
    CHECK(info.y == error.y);   // one line each
    CHECK(info.y < 40.0f);
}

TEST_CASE("on-screen debug: keyed lines update in place and messages expire")
{
    OffscreenUi ui;
    AddOnScreenDebugMessage(7, 0.0f, DebugMessageType::Info, "frame 1");
    AddOnScreenDebugMessage(-1, 0.5f, DebugMessageType::Warning, "half a second");
    ui.Frame();
    REQUIRE(ui.Messages().size() == 2);

    AddOnScreenDebugMessage(7, 0.0f, DebugMessageType::Info, "frame 2");
    ui.Frame();
    std::vector<Rml::Element*> messages = ui.Messages();
    REQUIRE(messages.size() == 2);
    CHECK(messages[1]->GetInnerRML() == "frame 2");   // kept its place under the newer warning

    ui.Frame();   // not sent again: gone
    REQUIRE(ui.Messages().size() == 1);
    ui.Frame(0.6f);
    CHECK(ui.Messages().empty());

    AddOnScreenDebugMessage(-1, 5.0f, DebugMessageType::Error, "<b>not markup</b> & more");
    ui.Frame();
    messages = ui.Messages();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]->GetNumChildren() == 1);   // one text node, no <b> element
    ClearOnScreenDebugMessages();
    ui.Frame();
    CHECK(ui.Messages().empty());
}
#endif

TEST_CASE("ui: documents are translated through the active .po file")
{
    kestest::TempDir dir;
    const std::filesystem::path po = dir.Path("localization") / "pl.po";
    std::filesystem::create_directories(po.parent_path());
    const std::string text =
        "msgid \"MainMenu.NewGame\"\nmsgstr \"Nowa gra\"\n"
        "msgid \"HUD.Score\"\nmsgstr \"Wynik: {{score}}\"\n";
    kestest::WriteBytes(po.string(), reinterpret_cast<const uint8_t*>(text.data()), text.size());
    SetAssetRootOverride(dir.Root().string().c_str());
    REQUIRE(LocalizationSetLanguage("pl"));

    {
        OffscreenUi ui;
        Rml::DataModelConstructor model = ui.ui.GetContext()->CreateDataModel("hud");
        int score = 42;
        REQUIRE(model.Bind("score", &score));
        Rml::ElementDocument* document = ui.ui.LoadDocumentFromMemory(
            "<rml><head><title>#MainMenu.NewGame</title></head><body data-model=\"hud\">"
            "<p id=\"a\">#MainMenu.NewGame</p>"
            "<p id=\"b\">\n    #HUD.Score\n</p>"
            "<p id=\"c\">New Game</p>"
            "<p id=\"d\">#Missing.Key</p>"
            "</body></rml>");
        REQUIRE(document != nullptr);
        ui.Frame();
        CHECK(document->GetTitle() == "Nowa gra");
        CHECK(document->GetElementById("a")->GetInnerRML() == "Nowa gra");
        CHECK(document->GetElementById("b")->GetInnerRML().find("Wynik: 42") != Rml::String::npos);
        CHECK(document->GetElementById("c")->GetInnerRML() == "New Game");
        CHECK(document->GetElementById("d")->GetInnerRML() == "#Missing.Key");
    }
    LocalizationShutdown();
}

TEST_CASE("ui: gamepad text field callback fires on focus and accept")
{
    InputState input;
    OffscreenUi ui(&input);
    ui.ui.SetInputEnabled(true);
    ui.ui.SetNavigationEnabled(true);

    std::vector<UiTextFieldRequest> requests;
    ui.ui.SetGamepadTextFieldCallback([&](const UiTextFieldRequest& request) { requests.push_back(request); });

    Rml::ElementDocument* document = ui.ui.LoadDocumentFromMemory(
        "<rml><head><link type=\"text/rcss\" href=\"/engine/ui/kes.rcss\"/></head><body>"
        "<div><button id=\"play\">Play</button></div>"
        "<div><input id=\"seed\" type=\"number\" value=\"42\" maxlength=\"6\" placeholder=\"Seed\"/></div>"
        "<div><textarea id=\"notes\">hi</textarea></div>"
        "</body></rml>");
    REQUIRE(document != nullptr);

    const auto press = [&](const char* action)
    {
        input.actionStates[action] = { true, true, false };
        ui.Frame();
        input.actionStates[action] = {};
        ui.Frame();
    };

    // Keyboard: no callback.
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    ui.ui.ProcessEvent(key);
    ui.Frame();
    press("UI_Accept");   // focuses #play
    REQUIRE(ui.ui.GetContext()->GetFocusElement() == document->GetElementById("play"));
    press("UI_Down");
    REQUIRE(ui.ui.GetContext()->GetFocusElement() == document->GetElementById("seed"));
    CHECK(requests.empty());

    SDL_Event pad{};
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    ui.ui.ProcessEvent(pad);
    REQUIRE(ui.ui.GetActiveInputDevice() == UiInputDevice::Gamepad);

    press("UI_Accept");
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].trigger == UiTextFieldTrigger::Activated);
    CHECK(requests[0].kind == UiTextFieldKind::Number);
    CHECK(requests[0].value == "42");
    CHECK(requests[0].placeholder == "Seed");
    CHECK(requests[0].maxLength == 6);
    CHECK(requests[0].width > 0.0f);
    CHECK(requests[0].height > 0.0f);

    press("UI_Down");
    REQUIRE(ui.ui.GetContext()->GetFocusElement() == document->GetElementById("notes"));
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].trigger == UiTextFieldTrigger::Focused);
    CHECK(requests[1].kind == UiTextFieldKind::Multiline);
    CHECK(requests[1].value == "hi");

    press("UI_Accept");   // replaces Enter: no newline added
    REQUIRE(requests.size() == 3);
    CHECK(rmlui_dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("notes"))->GetValue() == "hi");

    press("UI_Up");
    press("UI_Up");
    REQUIRE(ui.ui.GetContext()->GetFocusElement() == document->GetElementById("play"));
    CHECK(requests.size() == 4);   // passing #seed fired Focused, #play didn't
}

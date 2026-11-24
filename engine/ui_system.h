#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "render_backend.h"

namespace Rml
{
    class Context;
    class ElementDocument;
    class Element;
    class Event;
}

struct InputState;

using UiActionCallback = std::function<void(Rml::Event& event, const std::string& args)>;
using UiSoundCallback = std::function<void(const std::string& path)>;

enum class UiTextFieldKind
{
    Text,
    Password,
    Number,
    Multiline
};

enum class UiTextFieldTrigger
{
    Focused,    // gamepad navigation moved focus onto the field
    Activated   // UI_Accept pressed on the focused field
};

// element is only valid during the callback; keep element->GetObserverPtr() to write a result back later
// (e.g. Rml::ElementFormControl::SetValue after a platform on-screen keyboard closes).
struct UiTextFieldRequest
{
    Rml::Element* element = nullptr;
    UiTextFieldKind kind = UiTextFieldKind::Text;
    UiTextFieldTrigger trigger = UiTextFieldTrigger::Activated;
    std::string value;
    std::string placeholder;
    int maxLength = -1;
    // Border box in context pixels.
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

using UiTextFieldCallback = std::function<void(const UiTextFieldRequest& request)>;

enum class UiInputDevice
{
    Mouse,
    Keyboard,
    Gamepad
};

enum class UiNavigationMode
{
    Never,
    UIOnly,
    GameAndUI
};

enum class UiAutoFocus
{
    Never,
    Gamepad,
    Always
};

struct UiConfig
{
    std::vector<std::string> fonts = {
        "engine/ui/fonts/LatoLatin-Regular.ttf",
        "engine/ui/fonts/LatoLatin-Bold.ttf",
        "engine/ui/fonts/LatoLatin-Italic.ttf",
        "engine/ui/fonts/LatoLatin-BoldItalic.ttf",
    };
    std::vector<std::string> fallbackFonts;
    float densityScale = 1.0f;

    UiNavigationMode navigation = UiNavigationMode::UIOnly;
    UiAutoFocus autoFocus = UiAutoFocus::Gamepad;
    bool rememberFocus = true;
    bool hideCursorWithGamepad = true;
    float repeatDelay = 0.40f;
    float repeatInterval = 0.11f;
    float stickPressThreshold = 0.50f;
    float stickReleaseThreshold = 0.35f;
    float scrollDeadzone = 0.20f;
    float scrollSpeed = 900.0f;

    std::string focusSound;
    std::string acceptSound;
    std::string backSound;
};

std::string GetDefaultUiConfigPath();
UiConfig LoadUiConfig(const std::string& path, bool* outFound = nullptr);

extern const char* const kUiDefaultActionMap;

class UiSystem
{
public:
    UiSystem();
    ~UiSystem();
    UiSystem(const UiSystem&) = delete;
    UiSystem& operator=(const UiSystem&) = delete;
    bool Init(SDL_Window* window, IRenderer* renderer, int pixelWidth, int pixelHeight,
        InputState* input = nullptr, const UiConfig& config = {});
    void Shutdown();
    bool IsInitialized() const;

    void SetConfig(const UiConfig& config);
    const UiConfig& GetConfig() const;

    bool ProcessEvent(const SDL_Event& event);
    void SetInputEnabled(bool enabled);
    bool IsInputEnabled() const;
    void SetNavigationEnabled(bool enabled);
    bool IsNavigationEnabled() const;

    UiInputDevice GetActiveInputDevice() const;

    bool WasBackPressed() const;
    bool WasConfirmPressed() const;
    void BeginFrame();

    void SetSoundCallback(UiSoundCallback callback);
    // Fires when a text/password/number field or textarea is focused or activated with the gamepad.
    void SetGamepadTextFieldCallback(UiTextFieldCallback callback);
    void SetViewportSize(int pixelWidth, int pixelHeight);
    void SetDensityScale(float scale);

    void Update();
    void Render();

    Rml::ElementDocument* LoadDocument(const std::string& path, bool show = true);
    Rml::ElementDocument* LoadDocumentFromMemory(const std::string& rml, const std::string& sourceUrl = "[memory]", bool show = true);
    void CloseDocument(Rml::ElementDocument* document);
    void CloseAllDocuments();
    void ReloadDocuments();

    bool LoadFontFace(const std::string& path, bool fallback = false);
    void RegisterAction(const std::string& name, UiActionCallback callback);
    void UnregisterAction(const std::string& name);
    bool WantsTextInput() const;
    bool IsPointerOverUi() const;
    void SetDebuggerVisible(bool visible);
    bool IsDebuggerVisible() const;

    Rml::Context* GetContext() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

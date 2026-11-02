#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "render_backend.h"

// HTML/CSS user interface built on RmlUi (https://github.com/mikke89/RmlUi).
//
// Documents are XHTML-like .rml files styled with .rcss (a CSS2 subset plus
// flexbox, transitions, animations, transforms, gradients...). There is no
// scripting: behaviour lives in C++, wired up through
//   - actions:      <button onclick="start_game">  ->  ui.RegisterAction("start_game", ...)
//   - data models:  {{ health }}, data-if, data-for ...  ->  ui.GetContext()->CreateDataModel(...)
//   - the full Rml API on the returned elements and documents.
//
// Paths go through the engine file system (loose content or mounted paks),
// relative to the content directory. Inside documents, relative paths resolve
// against the document, and paths starting with '/' against the content root
// (e.g. <link type="text/rcss" href="/engine/ui/kes.rcss"/>).
//
// Per-project settings (fonts, navigation, sounds) come from
// config/DefaultUi.ini, see UiConfig. The engine drives the system
// (EngineStart / EngineTick); game code only loads documents and reacts to
// events. See docs/ui-system.md.

namespace Rml
{
    class Context;
    class ElementDocument;
    class Element;
    class Event;
}

struct InputState;

// Called for onclick="name args..." (or any on<event> attribute). `args` is
// the attribute text after the action name, trimmed (may be empty).
using UiActionCallback = std::function<void(Rml::Event &event, const std::string &args)>;
// Plays a UI sound (UiConfig sound paths); the engine routes it to the audio system.
using UiSoundCallback = std::function<void(const std::string &path)>;

// The device the player used last. Documents get a matching class on <body>
// (input-mouse, input-keyboard, input-gamepad) to show the right prompts.
enum class UiInputDevice
{
    Mouse,
    Keyboard,
    Gamepad
};

// Input modes in which the UI_* input actions drive the UI.
enum class UiNavigationMode
{
    Never,
    UIOnly,      // InputMode::UIOnly (default)
    GameAndUI    // InputMode::UIOnly and InputMode::GameAndUI
};

// When a document without a focused control gets one automatically (the
// remembered control when RememberFocus is on, else its first control).
enum class UiAutoFocus
{
    Never,     // only on the first navigation input
    Gamepad,   // while the gamepad is the active device (default)
    Always
};

// config/DefaultUi.ini. Missing keys (or a missing file) keep these defaults.
struct UiConfig
{
    // [UI]
    // Font faces loaded at startup (content paths). The first one is also the
    // fallback for glyphs missing from other fonts.
    std::vector<std::string> fonts = {
        "engine/ui/fonts/LatoLatin-Regular.ttf",
        "engine/ui/fonts/LatoLatin-Bold.ttf",
        "engine/ui/fonts/LatoLatin-Italic.ttf",
        "engine/ui/fonts/LatoLatin-BoldItalic.ttf",
    };
    std::vector<std::string> fallbackFonts;   // extra fallback faces (e.g. CJK, symbols)
    float densityScale = 1.0f;                // multiplies the window display scale for dp units

    // [Navigation]
    UiNavigationMode navigation = UiNavigationMode::UIOnly;
    UiAutoFocus autoFocus = UiAutoFocus::Gamepad;
    bool rememberFocus = true;           // restore a document's last focused control when it comes back
    bool hideCursorWithGamepad = true;
    float repeatDelay = 0.40f;           // seconds before a held direction repeats
    float repeatInterval = 0.11f;        // seconds between repeats
    float stickPressThreshold = 0.50f;   // UI_Navigate tilt that counts as a press
    float stickReleaseThreshold = 0.35f; // ... and as a release (hysteresis)
    float scrollDeadzone = 0.20f;        // UI_Scroll
    float scrollSpeed = 900.0f;          // dp per second at full tilt

    // [Sounds] content paths, empty = silent
    std::string focusSound;    // focus moved by navigation
    std::string acceptSound;   // UI_Accept activated something
    std::string backSound;     // UI_Back
};

// <exe dir>/config/DefaultUi.ini
std::string GetDefaultUiConfigPath();
// Reads `path` over the defaults. `outFound` is false when the file is missing.
UiConfig LoadUiConfig(const std::string &path, bool *outFound = nullptr);

// Default bindings (input map syntax) for the UI actions. The engine adds the
// ones a project's input map doesn't define (InputAddDefaultActions):
//   UI_Up / UI_Down / UI_Left / UI_Right   boolean   D-pad, arrow keys
//   UI_Navigate                            vector2   left stick
//   UI_Accept                              boolean   A, Enter
//   UI_Back                                boolean   B, Start, Escape
//   UI_PrevTab / UI_NextTab                boolean   LB / RB
//   UI_Scroll                              vector2   right stick (y scrolls)
extern const char *const kUiDefaultActionMap;

class UiSystem
{
public:
    UiSystem();
    ~UiSystem();
    UiSystem(const UiSystem &) = delete;
    UiSystem &operator=(const UiSystem &) = delete;

    // Engine lifecycle (called by engine.cpp). `input` provides the UI_*
    // action states for navigation (may be null: no navigation).
    bool Init(SDL_Window *window, IRenderer *renderer, int pixelWidth, int pixelHeight,
              InputState *input = nullptr, const UiConfig &config = {});
    void Shutdown();
    bool IsInitialized() const;

    // Replaces the configuration (e.g. after reloading DefaultUi.ini). New
    // fonts are loaded; fonts already loaded stay.
    void SetConfig(const UiConfig &config);
    const UiConfig &GetConfig() const;

    // Feeds an SDL event to the UI. Pointer/keyboard events are only used
    // while input is enabled (InputMode GameAndUI / UIOnly). Returns true when
    // the UI consumed the event (a click on an element, typing into a field),
    // so the caller can keep it away from game input.
    bool ProcessEvent(const SDL_Event &event);
    void SetInputEnabled(bool enabled);
    bool IsInputEnabled() const;

    // Navigation with the UI_* input actions (set by the engine from
    // UiConfig::navigation and the input mode):
    //   UI_Up/Down/Left/Right, UI_Navigate  move focus between focusable
    //                     elements (nav: auto in kes.rcss), repeating while held;
    //                     left/right also step a focused range slider
    //   UI_Accept         activates the focused element (like Enter)
    //   UI_Back           closes an open <select>, otherwise sends Escape and
    //                     reports WasBackPressed()
    //   UI_PrevTab/NextTab  switch tabs of the document's <tabset>
    //   UI_Scroll         scrolls the nearest scrollable container
    // Keys bound to UI actions are handled by the actions, not by RmlUi, except
    // while a text field has focus.
    void SetNavigationEnabled(bool enabled);
    bool IsNavigationEnabled() const;

    UiInputDevice GetActiveInputDevice() const;

    // Per-frame results of navigation input that no UI element used, so game
    // code can react to them in any input mode. Reset by BeginFrame.
    //   Back:    UI_Back, or the Escape key while navigation is off
    //   Confirm: UI_Accept with nothing focusable in the top-most document
    bool WasBackPressed() const;
    bool WasConfirmPressed() const;
    // Called by the engine before the event pump.
    void BeginFrame();

    void SetSoundCallback(UiSoundCallback callback);

    // Backbuffer size in pixels; UI coordinates are pixels.
    void SetViewportSize(int pixelWidth, int pixelHeight);
    // Scale of 'dp' units. The engine sets the window display scale times
    // UiConfig::densityScale, and again when the window changes display.
    void SetDensityScale(float scale);

    void Update();   // navigation, layout, animations, data bindings
    void Render();   // between IRenderer::BeginBackbufferOverlay and Present

    // Documents
    Rml::ElementDocument *LoadDocument(const std::string &path, bool show = true);
    Rml::ElementDocument *LoadDocumentFromMemory(const std::string &rml, const std::string &sourceUrl = "[memory]", bool show = true);
    void CloseDocument(Rml::ElementDocument *document);
    void CloseAllDocuments();
    // Reloads every open document from disk (development hot reload).
    void ReloadDocuments();

    bool LoadFontFace(const std::string &path, bool fallback = false);

    // onclick="name ..." handlers. Registering an existing name replaces it.
    void RegisterAction(const std::string &name, UiActionCallback callback);
    void UnregisterAction(const std::string &name);

    // True while a text field / select has keyboard focus.
    bool WantsTextInput() const;
    // True while the pointer hovers a UI element (not the empty root).
    bool IsPointerOverUi() const;

    void SetDebuggerVisible(bool visible);
    bool IsDebuggerVisible() const;

    // Full RmlUi access: data models, element queries, custom elements...
    Rml::Context *GetContext() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

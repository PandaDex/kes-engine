#pragma once

#include <functional>
#include <memory>
#include <string>
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
// Fonts: engine/ui/fonts/*.ttf|otf are loaded at startup. Load more with
// LoadFontFace. The engine drives the system (EngineStart / EngineTick), game
// code only loads documents and reacts to events.

namespace Rml
{
    class Context;
    class ElementDocument;
    class Element;
    class Event;
}

// Called for onclick="name args..." (or any on<event> attribute). `args` is
// the attribute text after the action name, trimmed (may be empty).
using UiActionCallback = std::function<void(Rml::Event &event, const std::string &args)>;

class UiSystem
{
public:
    UiSystem();
    ~UiSystem();
    UiSystem(const UiSystem &) = delete;
    UiSystem &operator=(const UiSystem &) = delete;

    // Engine lifecycle (called by engine.cpp)
    bool Init(SDL_Window *window, IRenderer *renderer, int pixelWidth, int pixelHeight);
    void Shutdown();
    bool IsInitialized() const;

    // Feeds an SDL event to the UI. Pointer/keyboard events are only used
    // while input is enabled (InputMode GameAndUI / UIOnly). Returns true when
    // the UI consumed the event (a click on an element, typing into a field),
    // so the caller can keep it away from game input.
    bool ProcessEvent(const SDL_Event &event);
    void SetInputEnabled(bool enabled);
    bool IsInputEnabled() const;

    // Backbuffer size in pixels; UI coordinates are pixels.
    void SetViewportSize(int pixelWidth, int pixelHeight);
    // Scale of 'dp' units (window display scale, set automatically).
    void SetDensityScale(float scale);

    void Update();   // layout, animations, data bindings
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

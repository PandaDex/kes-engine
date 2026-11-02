# UI System

The engine UI is HTML/CSS based and built on [RmlUi](https://github.com/mikke89/RmlUi) 6.3, with [FreeType](https://freetype.org) 2.14.3 for fonts. Both are git submodules built from source as static libraries (`cmake/KesUi.cmake`), so the UI builds the same way on every platform the engine supports. There is no scripting language: documents describe layout and style, and behaviour lives in C++.

```
git submodule update --init external/RmlUi external/freetype
```

## Overview

- **Documents** are `.rml` files: XHTML-like markup (`<div>`, `<p>`, `<h1>`, `<button>`, `<input>`, `<select>`, `<table>`...).
- **Style sheets** are `.rcss` files: CSS2 selectors and box model, plus flexbox, transitions, keyframe animations, transforms, decorators (images, nine-patch, straight gradients) and `dp` units. RmlUi documents what is supported, and how it differs from browsers, in its [RCSS reference](https://mikke89.github.io/RmlUiDoc/pages/rcss.html).
- **Behaviour**: `onclick="action"` attributes call C++ actions, data models bind C++ variables into markup (`{{ health }}`, `data-if`, `data-for`, `data-value`...), and you can use the full Rml API on documents and elements.
- **Rendering**: UI geometry is batched into a `UiDrawList` and drawn by the active renderer (Vulkan or D3D11) at full window resolution, over the low-res scene (`engine/shaders/ui.slang`).
- **Files** come through the engine file system, as loose content or from mounted paks, so packaged builds need no extra steps.

## Files and Paths

| Path (content-relative) | Contents |
| --- | --- |
| `engine/ui/kes.rcss` | default style sheet (`engine/assets/engine/ui/kes.rcss`) |
| `engine/ui/fonts/*.ttf` | Lato fonts, family `LatoLatin` (SIL OFL, `LICENSE-Lato.txt`) |
| `ui/...` | game documents (`game/content/ui`) |

Path rules inside documents:

- relative paths (`href="game_ui.rcss"`, `src="icons/heart.ktex"`) resolve against the document
- paths starting with `/` resolve against the content root (`href="/engine/ui/kes.rcss"`)

RmlUi has no built-in browser defaults: every element starts out inline and unstyled. Link `kes.rcss` first to get HTML-like block elements, headings, form controls and scrollbars:

```html
<rml>
<head>
    <title>Main menu</title>
    <link type="text/rcss" href="/engine/ui/kes.rcss"/>
    <link type="text/rcss" href="menu.rcss"/>
</head>
<body>
    <h1>My Game</h1>
    <button onclick="start_game">Play</button>
    <button onclick="open_menu settings">Settings</button>
    <button onclick="quit">Quit</button>
</body>
</rml>
```

Images (`<img src>`, `decorator: image(...)`) use the engine texture loader: `.ktex` (import PNG/JPG with the model importer) or `.bmp`.

Units: `px` are backbuffer pixels, and `dp` are scaled by the window's display scale (HiDPI). Prefer `dp` for sizes.

## C++ API

The engine owns one `UiSystem` (`engine/ui_system.h`) in `EngineApp::ui`. It is initialized in `EngineStart`, updated and drawn every `EngineTick`, and shut down in `EngineShutdown`. Game code gets it with `EngineGetUi(app)`, or uses `app.ui` directly.

```cpp
#include <RmlUi/Core.h>
#include "engine.h"

UiSystem& ui = EngineGetUi(app);

// Actions: onclick="name args" -> callback(event, "args")
ui.RegisterAction("start_game", [&](Rml::Event&, const std::string&) { /* load map, hide menu */ });
ui.RegisterAction("open_menu", [&](Rml::Event&, const std::string& args) { /* args == "settings" */ });

// Documents
Rml::ElementDocument* menu = ui.LoadDocument("ui/main_menu.rml");          // shown
Rml::ElementDocument* pause = ui.LoadDocument("ui/pause.rml", false);      // hidden
pause->Show(Rml::ModalFlag::Modal);
pause->Hide();
ui.CloseDocument(menu);
```

| Function | Purpose |
| --- | --- |
| `LoadDocument(path, show)` / `LoadDocumentFromMemory(rml, url, show)` | load an `.rml` document |
| `CloseDocument(doc)` / `CloseAllDocuments()` | close documents |
| `ReloadDocuments()` | reload open documents and style sheets from disk (hot reload) |
| `RegisterAction(name, callback)` / `UnregisterAction(name)` | C++ handlers for `on<event>` attributes |
| `LoadFontFace(path, fallback)` | load more `.ttf`/`.otf` fonts |
| `WantsTextInput()` | a text field has keyboard focus |
| `IsPointerOverUi()` | the cursor is over a UI element (not an empty document body) |
| `SetDebuggerVisible(bool)` | RmlUi's element inspector (debug builds) |
| `GetContext()` | the `Rml::Context`, for data models, element queries and anything else in the Rml API |

### Actions

Any `on<event>` attribute (`onclick`, `onchange`, `onsubmit`, `onmouseover`, `onkeydown`...) is parsed as one or more actions separated by `;`. The first word is the action name and the rest of the statement is passed as `args`:

```html
<button onclick="play_sound click; set_difficulty hard">Hard</button>
<input type="range" min="0" max="100" onchange="set_volume music"/>
```

```cpp
ui.RegisterAction("set_volume", [&](Rml::Event& event, const std::string& channel) {
    const float value = event.GetParameter<float>("value", 0.0f);
    // channel == "music"
});
```

Actions are looked up when the event fires, so they can be registered before or after the document loads. If an action isn't registered, the UI logs a warning and nothing else happens.

### Data Binding

Create data models before loading the documents that use them:

```cpp
struct HudState { int health = 100; int ammo = 30; } hud;

Rml::DataModelConstructor model = ui.GetContext()->CreateDataModel("hud");
model.Bind("health", &hud.health);
model.Bind("ammo", &hud.ammo);
Rml::DataModelHandle hudModel = model.GetModelHandle();

ui.LoadDocument("ui/hud.rml");

// after changing values:
hud.health -= 10;
hudModel.DirtyVariable("health");
```

```html
<body data-model="hud">
    <div class="health" data-class-low="health < 25">{{ health }} HP</div>
    <div data-if="ammo == 0">Reload!</div>
</body>
```

The full binding syntax is in the RmlUi [data bindings docs](https://mikke89.github.io/RmlUiDoc/pages/data_bindings.html).

## Input

The UI gets mouse and keyboard input in `InputMode::GameAndUI` and `InputMode::UIOnly` (`EngineSetInputMode`). In `GameOnly`, the cursor is locked for mouse look, so the UI only gets window events.

Each event goes to the UI first. When the UI consumes it, game input (`InputState`) never sees it:

- mouse button presses and wheel scrolls over a UI element. The empty `<body>` of a full-screen document doesn't count, so a HUD doesn't block game clicks.
- key presses and text while a text field has focus, plus keys that a document handles itself
- releases (mouse buttons and keys) always reach the game too, so nothing gets stuck down

The SDL platform code handles text input and IME, clipboard, and cursor shapes (CSS `cursor: pointer | text | move | ...`).

Debug builds (`ENGINE_ALLOW_DEBUG_MODE`) add:

- **F7**: reload open documents and style sheets from disk
- **F8**: toggle the RmlUi debugger (element inspector, event log), switching to `GameAndUI` if needed

## Example

`game/src/main.cpp` and `game/content/ui` contain a working example:

- `hud.rml`: an FPS counter bound to the `hud` data model
- `pause_menu.rml`: a modal menu opened with `InputAction_Pause` (Escape / gamepad Start), with buttons wired to the `resume`, `toggle_fullscreen` and `quit` actions

## Rendering Details

- The UI is drawn after the scene blit, inside the backbuffer pass, at native window resolution (`IRenderer::RenderUi`).
- Vertices are in pixels with premultiplied-alpha colors. Blending is `ONE, ONE_MINUS_SRC_ALPHA` with linear filtering. Scissor regions handle `overflow: hidden` and scrolling.
- Transforms (`transform: rotate(...)`) are applied on the CPU.
- Textures (font atlases, images) go through `IRenderer::CreateUiTexture` / `DestroyUiTexture`. On Vulkan, destruction is deferred until frames in flight no longer use the texture.
- Supported decorators: `image`, `tiled-*`, `ninepatch`, `text`, and straight gradients (`horizontal-gradient`, `vertical-gradient`).
- Not supported yet, because they need render layers or custom shaders in the render interface: `linear-gradient` / `radial-gradient` / `conic-gradient` decorators, `box-shadow`, `filter` / `backdrop-filter`, `mask-image`, and overflow clipping of *transformed* elements (clip masks). RmlUi skips these silently, and the rest of the element still renders.

## Profiling

UI work appears under the Tracy category `UI` (`KES_PROFILE_UI`), and the GPU zone is named `UI`. See `profiling.md`.

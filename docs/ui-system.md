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
- elements dragged with `drag: clone` are copied into a document that has no path, so images inside them need root paths (`/ui/icons/medkit.bmp`). With a relative path the image fails to load and is drawn as a white box

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

### Navigation (keyboard and gamepad)

Menus are navigated with **UI input actions** from the project's input map (`config/default.imap`), so every project can rebind them:

| Action | Type | Default bindings | Effect |
|---|---|---|---|
| `UI_Up` `UI_Down` `UI_Left` `UI_Right` | boolean | D-pad, arrow keys | move focus, repeating while held; left/right also step a focused range slider |
| `UI_Navigate` | vector2 | left stick | same as the directions, with press/release thresholds |
| `UI_Accept` | boolean | A, Enter, keypad Enter | activate the focused element (Enter) |
| `UI_Back` | boolean | B, Start, Escape | close an open `<select>`, otherwise Escape, and `WasBackPressed()` |
| `UI_PrevTab` `UI_NextTab` | boolean | LB / RB | switch tabs of the document's `<tabset>`, focusing the new panel's first control |
| `UI_Scroll` | vector2 | right stick | scroll the nearest scrollable container (y) |

- Actions named `UI_*` belong to the UI context (or set `"Context": "UI"` on any action). They only read while navigation is on, and game actions read as idle in `UIOnly` as before. A button held as navigation turns on (e.g. Start opening the pause menu) doesn't count as a press.
- The engine adds every UI action the project's map doesn't define (`kUiDefaultActionMap` in `ui_system.cpp`). To change one, define it in the project map. An action with an empty `NativeKeys` list disables it.
- Keys bound to UI actions are handled by the actions (the same repeat timing as the gamepad), not by RmlUi, except while a text field has focus: then every key goes to the field and UI keyboard bindings are ignored. Gamepad up/down leaves a text field along the tab order.
- All connected gamepads are opened, and the one used last is the one bindings read.

Focusable elements need `tab-index: auto`, and arrow/gamepad navigation needs `nav: auto` (or `nav-up` / `nav-down` / ... with an element id). `kes.rcss` sets both on buttons and form controls, and `[disabled]` elements are skipped. Add them to custom widgets (e.g. an inventory slot `div`) and give those a `:focus-visible` style. Tab / Shift+Tab always work natively.

**Focus handling:** `ElementDocument::Show` only focuses the document itself. When a document has no focused control, the UI focuses its remembered control (the last one focused there), or else its first non-text-field control. That happens on the first navigation input, and with `AutoFocus=Gamepad` also automatically while the gamepad is active (including when the focused control is hidden or removed). The navigation target is the document holding focus, or the top-most visible document with a control.

**Input device:** the device used last (`UiSystem::GetActiveInputDevice`) is set as a class on every document's `<body>`: `input-mouse`, `input-keyboard` or `input-gamepad`. Use it for prompts:

```css
.pad-only { display: none; }
body.input-gamepad .pad-only { display: inline; }
body.input-gamepad .kbm-only { display: none; }
```

While the gamepad navigates, the mouse cursor is hidden (`HideCursorWithGamepad`) and hover styles are cleared. Moving the mouse brings the cursor back.

Navigation input that no element used is reported for the current frame, so game code doesn't check keys or buttons itself:

```cpp
if (ui.WasBackPressed())      // UI_Back (or Escape while navigation is off)
    CloseTopMenu();
if (ui.WasConfirmPressed())   // UI_Accept with nothing focusable, e.g. to skip dialogue text
    SkipText();
```

## Project Configuration

Each project ships `config/DefaultUi.ini` next to the exe (`kes_add_game(... CONFIG_DIR ...)`, the same folder as `default.imap` and `DefaultGameSettings.ini`). Missing keys, or a missing file, keep the engine defaults (`UiConfig` in `ui_system.h`). In debug builds, F7 reloads it together with the documents.

```ini
[UI]
Fonts=engine/ui/fonts/LatoLatin-Regular.ttf, engine/ui/fonts/LatoLatin-Bold.ttf, ...   ; first = fallback
FallbackFonts=                ; extra fallback faces (CJK, symbols)
DensityScale=1.0              ; multiplies the window display scale for dp

[Navigation]
GamepadNavigation=UIOnly      ; input modes where UI_* actions drive the UI: UIOnly, GameAndUI, Never
AutoFocus=Gamepad             ; Gamepad, Always or Never (only on the first input)
RememberFocus=True
HideCursorWithGamepad=True
RepeatDelay=0.40              ; seconds before a held direction repeats
RepeatInterval=0.11
StickPressThreshold=0.50      ; UI_Navigate tilt for a press / a release
StickReleaseThreshold=0.35
ScrollDeadzone=0.20           ; UI_Scroll
ScrollSpeed=900               ; dp per second

[Sounds]                      ; content paths, played through the world's audio system
Focus=
Accept=
Back=
```

`GameAndUI` lets the UI actions run alongside game actions. Bindings shared by both (A for Interact and Accept) then do both, so pick distinct bindings in that case. `game/config` and `samples/ui/config` are the two example projects. The sample overrides `UI_Accept` (adding Space) and adds gamepad bindings for its gameplay actions.

## Example

`game/src/main.cpp` and `game/content/ui` contain a working example:

- `hud.rml`: an FPS counter bound to the `hud` data model
- `pause_menu.rml`: a modal menu opened with `InputAction_Pause` (Escape / gamepad Start), with buttons wired to the `resume`, `toggle_fullscreen` and `quit` actions

`samples/ui` (target `ui_showcase`) is a bigger example: a small game with a main menu, loading screen, HUD, inventory with drag and drop, branching dialogue, a chat console, pause menu and tabbed settings, all driven by one data model. See [samples/ui/README.md](../samples/ui/README.md).

## Rendering Details

- The UI is drawn after the scene blit, inside the backbuffer pass, at native window resolution (`IRenderer::RenderUi`).
- Vertices are in pixels with premultiplied-alpha colors. Blending is `ONE, ONE_MINUS_SRC_ALPHA` with linear filtering. Scissor regions handle `overflow: hidden` and scrolling.
- Transforms (`transform: rotate(...)`) are applied on the CPU.
- Textures (font atlases, images) go through `IRenderer::CreateUiTexture` / `DestroyUiTexture`. On Vulkan, destruction is deferred until frames in flight no longer use the texture.
- Supported decorators: `image`, `tiled-*`, `ninepatch`, `text`, and straight gradients (`horizontal-gradient`, `vertical-gradient`).
- Not supported yet, because they need render layers or custom shaders in the render interface: `linear-gradient` / `radial-gradient` / `conic-gradient` decorators, `box-shadow`, `filter` / `backdrop-filter`, `mask-image`, and overflow clipping of *transformed* elements (clip masks). RmlUi skips these silently, and the rest of the element still renders.
- Because clip masks aren't supported, an `overflow: hidden` box only clips with its scissor rectangle, and RmlUi only sets that when it detects overflowing content. Absolutely positioned or animated children may not count, so add `clip: always` to boxes that must always clip (see `#credits` in `samples/ui/content/ui/showcase.rcss`).

## Profiling

UI work appears under the Tracy category `UI` (`KES_PROFILE_UI`), and the GPU zone is named `UI`. See `profiling.md`.

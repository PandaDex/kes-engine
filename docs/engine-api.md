# Engine API

This document covers the main runtime-facing API used by game code.

The public entry point today is intentionally small. Most game code interacts with the engine through:

- `EngineApp`
- lifecycle functions in `engine.h`
- `World`
- `GameContext`
- `Entity`
- `AssetSystem`
- `Scene`
- `AudioSystem`
- `InputState`
- `UiSystem` (HTML/CSS UI, see [ui-system.md](ui-system.md))

## Minimal Bootstrap

The current game bootstrap is intentionally thin:

```cpp
#include "engine.h"
#include "file_paths.h"

int main(int argc, char* argv[])
{
    CollectPositionalCommandLineArguments(argc, argv);

    EngineApp app = {};

    if (!EngineStart(app))
    {
        EngineShutdown(app);
        return 1;
    }

    while (EngineTick(app))
    {
    }

    EngineShutdown(app);
    return 0;
}
```

`CollectPositionalCommandLineArguments` is used here so command-line `-assetpath` overrides are applied before startup.
`EngineSetCommandLine(argc, argv)` stores the command line for the engine (`command_line.h`). The engine reads these options:

| Option | Effect |
| --- | --- |
| `-renderer=vulkan\|d3d11` | renderer backend (when the platform allows choosing) |
| `-windowed`, `-borderless`, `-fullscreen` | window mode for this run |
| `-ResX=1280 -ResY=720` | resolution for this run |
| `-vsync`, `-novsync` | vsync for this run |

These overrides are never saved to the player's settings. A game can also set a preferred backend with `app.config.renderer = RendererBackend::D3D11;` before `EngineStart`.

## `EngineApp`

`EngineApp` is the main aggregate runtime state owned by the executable.

Important fields:

- `config`
- `window`
- `input`
- `renderer` (`std::unique_ptr<IRenderer>`, the backend chosen at startup; see "Renderer backends" in `tools-and-pipeline.md`)
- `camera`
- `sceneState`
- `world`
- `ui` (`UiSystem`, also via `EngineGetUi(app)`)
- `console`

Main lifecycle functions:

- `bool EngineStart(EngineApp& app);`
- `bool EngineTick(EngineApp& app);`
- `void EngineShutdown(EngineApp& app);`

Typical usage:

- create a zero-initialized `EngineApp`
- optionally register custom entities/player class
- call `EngineStart`
- run `EngineTick` in a loop
- call `EngineShutdown`

## Window and Cursor

The window supports three modes (`WindowMode`, same values as Unreal's `EWindowMode`):

- `Fullscreen`: exclusive, changes the display mode to the closest supported resolution
- `WindowedFullscreen`: borderless window at desktop resolution
- `Windowed`: resizable window, centered on the selected display

Behaviour handled by the engine:

- **Alt+Enter** toggles between windowed and the preferred fullscreen mode (`EngineToggleFullscreen`), and saves the choice
- the renderer follows the window's pixel size, so HiDPI displays and display changes are handled
- **minimized:** no rendering, and the loop blocks on events instead of spinning
- **focus loss:** held keys are released and the mouse is freed
- **closing:** a window you resized by dragging remembers its size on exit

Input modes control the mouse (`EngineSetInputMode`, like Unreal's `SetInputMode*`):

| Mode | Cursor | Game input |
| --- | --- | --- |
| `InputMode::GameOnly` (default) | hidden, locked to the window, raw relative motion | everything |
| `InputMode::GameAndUI` | visible, free | keys, buttons and gamepad; no mouse look |
| `InputMode::UIOnly` | visible, free | input actions read as idle (menus) |

`EngineSetShowMouseCursor(app, false)` hides the cursor in the two UI modes, which are also the modes where UI documents get mouse and keyboard input (see [ui-system.md](ui-system.md#input)). In debug builds (`ENGINE_ALLOW_DEBUG_MODE`), **F4** switches between GameOnly and GameAndUI, **F7** reloads UI documents and **F8** toggles the UI debugger.

Display helpers in `window.h`: `GetDisplayCount()`, `GetDesktopResolution(display)`, `GetSupportedFullscreenResolutions(display)`.

## GameSettings

`GameSettings` (`engine/game_settings.h`) holds the player-facing engine settings and works like Unreal's `UGameUserSettings`. The engine creates it at startup; get it anywhere with `GetGameSettings()`.

| Setting | API |
| --- | --- |
| Resolution | `SetScreenResolution`, `GetScreenResolution`, `GetLastConfirmedScreenResolution`, `GetDesktopResolution`, `GetSupportedFullscreenResolutions` |
| Window mode | `SetFullscreenMode`, `GetLastConfirmedFullscreenMode`, `SetPreferredFullscreenMode` (used by Alt+Enter) |
| Display | `SetDisplayIndex` |
| VSync | `SetVSyncEnabled`, `IsVSyncEnabled` |
| Frame rate limit | `SetFrameRateLimit(fps)`, 0 = unlimited |
| Resolution scale | `SetResolutionScaleValue(percent)` / `SetResolutionScaleNormalized(0..1)`, scales the internal render resolution (`AppConfig::renderWidth/Height`) |
| Renderer | `SetPreferredRenderer`, takes effect on the next launch |

Applying and saving follows the Unreal flow:

```cpp
GameSettings* settings = GetGameSettings();
settings->SetScreenResolution({ 1920, 1080 });
settings->SetFullscreenMode(WindowMode::Fullscreen);
settings->ApplySettings(false);        // ApplyResolutionSettings + ApplyNonResolutionSettings + SaveSettings

// "Keep these settings?"
settings->ConfirmVideoMode();          // or: RevertVideoMode(); ApplySettings(false);
settings->SaveSettings();
```

Also available: `IsDirty`, `IsScreenResolutionDirty`, `IsFullscreenModeDirty`, `IsVSyncDirty`, `ResetToCurrentSettings`, `SetToDefaults`, `ValidateSettings` and `LoadSettings(forceReload)`.

Files, lowest priority first:

1. `AppConfig` values set in game code
2. `<exe>/config/DefaultGameSettings.ini`: project defaults, shipped with the game
3. `<user config dir>/Config/GameSettings.ini`: the player's settings, from `SDL_GetPrefPath(KES_PROJECT_PUBLISHER, KES_PROJECT_NAME)` (`%APPDATA%\<publisher>\<name>\` on Windows)

Bumping `GameSettings::kSettingsVersion` resets old player files to defaults.

Custom settings, option 1, key/value pairs stored in `[CustomSettings]`:

```cpp
settings->SetCustomFloat("MasterVolume", 0.8f);
float volume = settings->GetCustomFloat("MasterVolume", 1.0f);
```

Custom settings, option 2, a subclass (like overriding `UGameUserSettings`):

```cpp
class MySettings : public GameSettings
{
public:
    float masterVolume = 1.0f;
protected:
    void SetToDefaults() override { GameSettings::SetToDefaults(); masterVolume = 1.0f; }
    void LoadCustomSettings(const IniFile& ini) override { masterVolume = ini.GetFloat("MySettings", "MasterVolume", masterVolume); }
    void SaveCustomSettings(IniFile& ini) const override { ini.SetFloat("MySettings", "MasterVolume", masterVolume); }
};

EngineSetGameSettingsClass<MySettings>(app);   // before EngineStart
GetGameSettings<MySettings>()->masterVolume = 0.5f;
```

## Entity Registration

The engine supports runtime registration of entity factories by class name.

Functions:

- `void EngineRegisterEntityClass(EngineApp& app, const std::string& className, EntityFactoryFunction factory);`
- `void EngineSetPlayerClass(EngineApp& app, const std::string& className);`
- `template<typename T> void EngineRegisterEntityClass(EngineApp& app, const std::string& className);`
- `template<typename T> void EngineRegisterPlayerClass(EngineApp& app, const std::string& className);`

Use this when you want map files to spawn your own entity classes by name.

`EngineRegisterPlayerClass<T>` is the convenience path for a player entity that is constructed with `Camera&` and `InputState&`.

## `World`

`World` is the main gameplay/runtime container.

Responsibilities:

- owns scene state
- owns asset system
- owns collision world
- owns audio system
- owns entity manager
- loads maps and prefabs
- updates entities

Important functions:

- `bool Init(Camera* mainCamera = nullptr, InputState* input = nullptr);`
- `void Shutdown();`
- `void Update(float dt);`
- `void FixedUpdate(float fixedDt);`
- `bool LoadMap(const char* mapName);`
- `void QueueMapLoad(const std::string& mapName);`
- `void RegisterEntityClass(const std::string& className, EntityFactoryFunction factory);`
- `void SetPlayerClassName(const std::string& className);`

Main accessors:

- `Scene& GetScene();`
- `AssetSystem& GetAssets();`
- `GameContext& GetGameContext();`
- `EntityManager& GetEntities();`

## `GameContext`

`GameContext` is the object passed into entities indirectly through `Entity::game`.

It gives entities access to the rest of the runtime without making everything global.

Available systems:

- `world`
- `scene`
- `assets`
- `audio`
- `collision`
- `entities`
- `input`
- `mainCamera`

Important helper functions:

- `void Play2DSound(const char* soundFile, float volume = 1.0f, float pitch = 1.0f);`
- `void Play3DSound(const char* soundFile, const Vector3& location, float attenuation, float volume = 1.0f, float pitch = 1.0f);`
- `bool LoadMap(const char* mapName);`
- `bool SetActiveCameraEntity(uint64_t entityId);`
- `bool SetActiveCameraByTag(const std::string& tag);`
- `bool ExecuteCommand(const std::string& command, Entity* self = nullptr, Entity* other = nullptr);`

Convenience template queries:

- `GetEntityOfClass<T>()`
- `GetEntityOfClassWithTag<T>(...)`

## `Entity`

`Entity` is the current base class for gameplay objects.

Core data:

- `Position`
- `Rotation`
- `Tags`

Lifecycle hooks:

- `virtual void Start()`
- `virtual void Update(float dt)`
- `virtual void FixedUpdate(float fixedDt)`
- `virtual void Teleport(const Vector3& newPosition)`
- `virtual bool ApplyProperty(const std::string& name, const std::string& value)`

Lifetime helpers:

- `uint64_t GetEntityId() const`
- `bool HasTag(const std::string& tag) const`
- `bool IsPendingDestroy() const`
- `void Destroy()`

How entities are used today:

- map files instantiate them by class name
- string properties from maps are applied through `ApplyProperty`
- they update through the world/entity manager each frame

## `EntityManager`

`EntityManager` owns live entity instances.

Important functions:

- `CreateEntity<T>(...)`
- `StartPending()`
- `Update(float dt)`
- `FixedUpdate(float fixedDt)`
- `DestroyPending()`
- `DestroyAll()`
- `GetEntityById(...)`
- `GetEntityOfClass<T>()`
- `GetEntityOfClassWithTag<T>(...)`
- `ForEachActive(...)`

The engine currently uses an inheritance-based entity model, not an ECS.

## `AssetSystem`

`AssetSystem` is the runtime asset cache/loader for render and animation assets.

Supported runtime loads:

- meshes
- models
- shaders
- materials
- animation clips
- animation graphs

Important functions:

- `MeshHandle LoadMesh(const char* path);`
- `ModelHandle LoadModel(const char* path);`
- `ShaderHandle LoadShader(const char* path);`
- `MaterialHandle LoadMaterial(const char* path);`
- `AnimationClipHandle LoadAnimationClip(const char* path);`
- `AnimationGraphHandle LoadAnimationGraph(const char* path);`
- `void Clear();`

Lookup functions:

- `GetMesh(...)`
- `GetModel(...)`
- `GetShader(...)`
- `GetMaterial(...)`
- `GetAnimationClip(...)`
- `GetAnimationGraph(...)`

Notes:

- paths are asset-relative
- repeated loads are cached by path
- textures and sounds are currently loaded by other subsystems, not through `AssetSystem`

## `Scene`

`Scene` owns render-visible objects and lights.

Important object/light functions:

- `uint32_t CreateObject(const SceneObject& object);`
- `bool RemoveObject(uint32_t id);`
- `SceneObject* FindObject(uint32_t id);`
- `uint32_t CreateLight(const Light& light);`
- `bool RemoveLight(uint32_t id);`
- `Light* FindLight(uint32_t id);`
- `void Clear();`

Stored scene state includes:

- object list
- light list
- sky settings
- fog settings

This is a runtime scene representation, not a full authored scene graph editor API.

## `AudioSystem`

`AudioSystem` provides simple clip caching and voice playback.

Important functions:

- `bool Init();`
- `void Shutdown();`
- `void Update(float dt);`
- `bool LoadClip(const char* soundFile);`
- `AudioVoiceId Play2D(const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false);`
- `AudioVoiceId Play3D(const Vector3& location, float attenuation, const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false);`
- `void StopVoice(AudioVoiceId id);`
- `bool IsVoicePlaying(AudioVoiceId id) const;`
- `void UpdateVoice3D(AudioVoiceId id, const Vector3& location, float attenuation);`
- `void SetVoicePitch(AudioVoiceId id, float pitch);`
- `void SetVoiceVolume(AudioVoiceId id, float volume);`

Current behavior:

- loads `.ksound`
- caches clips by path
- supports 2D and simple positional playback
- mixes to the current SDL audio device format

## `UiSystem`

HTML/CSS user interface (RmlUi). The engine owns it in `EngineApp::ui` and drives its lifecycle; game code loads documents and reacts to them:

- `Rml::ElementDocument* LoadDocument(const std::string& path, bool show = true);`
- `void CloseDocument(Rml::ElementDocument* document);`
- `void RegisterAction(const std::string& name, UiActionCallback callback);` for `onclick="name args"`
- `Rml::Context* GetContext() const;` for data models and the rest of the Rml API

Full guide: [ui-system.md](ui-system.md).

## `InputState`

`InputState` is the current low-level input snapshot.

Stored input data:

- keyboard state
- previous keyboard state
- mouse position
- mouse delta
- mouse button state
- previous mouse button state
- `quitRequested`

Functions:

- `void InputInit(InputState& input);`
- `void InputBeginFrame(InputState& input);`
- `void InputProcessEvent(InputState& input, const SDL_Event& e);`
- `bool InputKeyDown(const InputState& input, SDL_Scancode key);`
- `bool InputKeyPressed(const InputState& input, SDL_Scancode key);`
- `bool InputKeyReleased(const InputState& input, SDL_Scancode key);`
- `bool InputMouseDown(const InputState& input, int button);`
- `bool InputMousePressed(const InputState& input, int button);`

This is a direct state API. There is no higher-level action mapping layer yet.

## Configuration Macros

The main compile-time defaults live in `engine_configuration.h`.

Important macros:

- `ENGINE_PROJECT_NAME`
- `ENGINE_CONTENT_DIRECTORY`
- `ENGINE_PAK_ENCRYPTION_KEY_HEX`
- `ENGINE_STARTUP_MAP`
- `ENGINE_ALLOW_CONSOLE`
- `ENGINE_ALLOW_DEBUG_MODE`
- `ENGINE_DEFAULT_PLAYER_CLASS_NAME`

These act as engine defaults and can be overridden at compile time.

## Current API Shape

The runtime API is intentionally small and practical. It is not a stable SDK yet. Expect signatures and responsibilities to move as the engine grows.

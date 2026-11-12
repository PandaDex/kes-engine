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

`EngineSetShowMouseCursor(app, false)` hides the cursor in the two UI modes, which are also the modes where UI documents get mouse and keyboard input (see [ui-system.md](ui-system.md#input)). The cursor also hides while a gamepad navigates the UI (`DefaultUi.ini`).

**Pause:** `EngineSetPaused(app, true)` stops entity updates and pauses audio output, while rendering and the UI keep running (`World::SetPaused`). A pause menu usually pairs it with `InputMode::UIOnly`, as `game/src/main.cpp` does.

**Debug hotkeys** (`ENGINE_ALLOW_DEBUG_MODE`, CMake option `KES_DEBUG_TOOLS`, on by default, off in the `package-*` presets):
- **F1** wireframe, **F3** animation debug
- **F4** switches between GameOnly and GameAndUI
- **F7** reloads UI documents and `DefaultUi.ini`
- **F5** / **F6** (example game) play a sound to the right / left of the camera
- **F8** toggles the UI inspector

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
| Look controls | `SetMouseSensitivity`, `SetGamepadLookSensitivity` (multipliers, 0.05-10), `SetLookYInverted`; used live by the default player |
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

`EngineRegisterPlayerClass<T>` registers a player entity and makes it the player class. `T` is constructed from `(Camera&, InputState&)`, `(InputState&)` or `()`, whichever it supports, so a class derived from `EnginePlayerEntity` works directly.

### Default player (`EnginePlayerEntity`)

A first-person fly camera with a collision capsule, moved by `CollisionWorld::MoveCapsule`. It slides along walls and can move out if it starts inside geometry. Its capsule is kinematic, so it pushes dynamic bodies. It reads these input actions from `config/default.imap`:

| Action | Type | Default bindings |
|---|---|---|
| `InputAction_Move` | vector2 | WASD, left stick (analog: partial tilt = partial speed) |
| `InputAction_MoveUp` / `InputAction_MoveDown` | boolean | Space / Left Ctrl, RB / LB |
| `InputAction_Look` | vector2 | mouse (deltas: an angle per frame) |
| `InputAction_LookStick` | vector2 | right stick (a turn rate, squared response curve) |

Look speed is scaled by `GameSettings` (`MouseSensitivity`, `GamepadLookSensitivity`, `bInvertLookY`). For base values and noclip, see `SetMoveSpeed`, `SetMouseLookScale`, `SetStickLookSpeed` and `SetCollisionEnabled`. Subclasses can reuse `MoveWithCollision` and `UpdateLook`.

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
- `bool LoadMap(const char* mapName);` loads synchronously. If the file can't be loaded, the current level stays and `false` is returned.
- `void QueueMapLoad(const std::string& mapName);` loads at the end of the next `Update` (safe from inside entity code).
- `const std::string& GetCurrentMapName() const;`
- `void SetPaused(bool paused);` / `bool IsPaused() const;` stop entity updates and pause audio.
- `void RegisterEntityClass(const std::string& className, EntityFactoryFunction factory);`
- `void SetPlayerClassName(const std::string& className);`

Main accessors:

- `Scene& GetScene();`
- `AssetSystem& GetAssets();`
- `AudioSystem& GetAudio();`
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
- `const Guid& GetGuid() const`: the editor's GUID for map entities, random for ones created at runtime
- `bool HasTag(const std::string& tag) const`
- `bool IsPendingDestroy() const`
- `void Destroy()`

How entities are used today:

- map files instantiate them by class name
- string properties from maps are applied through `ApplyProperty`
- they update through the world/entity manager each frame

## MapScript

The editor's output -> input links (Outputs tab in Forge). `map_script.h`, `World::GetMapScript()`, `game->mapScript`.

An entity takes part by also implementing `IMapScriptable`:

```cpp
class DoorEntity : public Entity, public IMapScriptable
{
public:
    const MapScriptPorts& GetMapScriptPorts() const override
    {
        static const MapScriptPorts ports = { { { "Open" }, { "Close" } }, { { "OnOpened" } } };
        return ports;
    }

    bool AcceptInput(const std::string& input, const MapScriptCall& call) override
    {
        if (input == "Open") { Open(); FireOutput("OnOpened", call.activator); return true; }
        if (input == "Close") { Close(); return true; }
        return false;   // logged as unhandled
    }
};
```

- Declare the same ports as `input` / `output` lines on the class in the editor's `.dec` (`kes.dec` for built-ins) so they show up on the Outputs tab.
- `Entity::FireOutput(name, activator, value)` queues every link of that output. Inputs are delivered in `World::Update` after entity updates, after the link's delay; an input fired while delivering waits for the next update, so link loops can't hang a frame.
- `MapScriptCall` has the caller, the activator (e.g. the player that entered a trigger; either may be null if destroyed meanwhile) and the value: the link's parameter, or the output's value when the parameter is empty.
- Links target GUIDs. Map entities keep the GUID they got when placed in the editor; entities created by code get a random one (`EntityManager::GetEntityByGuid`, `SetEntityGuid`). Game code can add links with `MapScriptSystem::AddLink` or send an input directly with `SendInput`.
- Every scriptable entity also takes `Kill`. Links on entities that don't implement the interface, and unknown ports, are logged when the map loads.

Built-in ports:

| Class | Inputs | Outputs |
| --- | --- | --- |
| `TriggerEntity` | Enable, Disable, Toggle | OnStartTouch, OnEndTouch (activator = the entity) |
| `BrushEntity` | Enable, Disable, Toggle, Show, Hide, EnableCollision, DisableCollision | |
| `LightEntity` | TurnOn, TurnOff, Toggle, SetIntensity(float) | |
| `AudioSourceEntity` | Play, Stop, Toggle | |
| `CameraEntity` | Activate, Deactivate | |
| `FogVolumeEntity` | Enable, Disable, Toggle | |
| `LogicRelayEntity` | Trigger, Enable, Disable, Toggle | OnTrigger |
| `LogicAutoEntity` | | OnMapSpawn |

## `CollisionWorld`

Collision and rigid bodies, built on [Box3D](https://github.com/erincatto/box3d) (`external/box3d`). Box3D stays inside `collision_world.cpp`; entities reach this API through `game->collision`. `World::FixedUpdate` steps it after the entities' `FixedUpdate`, so a paused world doesn't simulate.

- Static colliders: `AddBox`, `AddSphere`, `AddCapsule`, `AddConvex`
- Rigid bodies: `AddRigidBox`, `AddRigidSphere`, `AddRigidHulls` with a `RigidBodyDesc` (motion, position, rotation, density, friction, restitution)
- Bodies: `SetPosition` (teleport), `SetTargetPosition` (kinematic, moves at a velocity), `SetActive`, `Remove`, `ApplyImpulse`, `SetLinearVelocity`, `GetLinearVelocity`
- Queries: `Raycast`, `RaycastAll`, `CheckBox`, `CheckSphere`, `CheckCapsule`, `SweepCapsule`. Triggers only appear in rays with `includeTriggers`
- `MoveCapsule`: character movement, slides along surfaces. Dynamic bodies don't stop it; a kinematic capsule following it pushes them
- `GetBodyPose(handle, game->fixedStepAlpha, ...)`: a dynamic body's pose between the last two steps, for smooth drawing at any frame rate

Dynamic bodies from a map:

- `BrushEntity`: `Dynamic`, `Density`, `Friction`, `Restitution`, `RollingResistance`
- `SkinnedModelEntity` / `StaticMeshEntity`: the same, plus `Collider` (`Box` or `Sphere`, fitted to the mesh bounds). Without `Dynamic` the collider is static.

The built-in `UseGravity` property is separate: it only drops an entity onto what is below it, with no rigid body.

## `EntityManager`

`EntityManager` owns live entity instances.

Important functions:

- `CreateEntity<T>(...)`
- `StartPending()`
- `Update(float dt)`
- `FixedUpdate(float fixedDt)`
- `DestroyPending()`
- `DestroyAll()`
- `GetEntityById(...)`, `GetEntityByGuid(...)`, `SetEntityGuid(...)`
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

- `void SetPaused(bool paused);` pauses the output device (voices keep their position)
- `void StopAll();`
- `bool HasOutput() const;`

Current behavior:

- loads `.ksound` and caches clips by path, converted once to the mixer format (float stereo, 48 kHz)
- mixes on SDL's audio thread (an `SDL_AudioStream` bound to the default playback device). SDL converts to the hardware format.
- 2D and simple positional playback (distance attenuation and stereo pan from the listener the player sets)
- at most `kMaxVoices` (128) voices at once. Further play calls return 0 and log a warning.
- without an output device (headless machine, no sound card), it runs silent instead of failing startup

## `UiSystem`

HTML/CSS user interface (RmlUi). The engine owns it in `EngineApp::ui` and drives its lifecycle; game code loads documents and reacts to them:

- `Rml::ElementDocument* LoadDocument(const std::string& path, bool show = true);`
- `void CloseDocument(Rml::ElementDocument* document);`
- `void RegisterAction(const std::string& name, UiActionCallback callback);` for `onclick="name args"`
- `Rml::Context* GetContext() const;` for data models and the rest of the Rml API

Full guide: [ui-system.md](ui-system.md).

## `InputState`

`InputState` (`input.h`) holds the raw input snapshot and the **input actions** loaded from the project's `config/default.imap`.

Raw state: `keys` / `previousKeys`, `mouseButtons` / `previousMouseButtons`, `mouseDeltaX/Y`, and the connected gamepads (`gamepad` is the one used last; all are opened).

Actions (preferred over raw keys, since players can rebind them and they cover keyboard and gamepad):

- `bool InputActionDown / InputActionPressed / InputActionReleased(const InputState&, const char* action);`
- `Vector2 InputActionValue2D(...)`, `Vector3 InputActionValue3D(...)`
- game actions read as idle in `InputMode::UIOnly`. UI actions (`UI_*`, `"Context": "UI"`) only read while the UI navigates (see [ui-system.md](ui-system.md#navigation-keyboard-and-gamepad)).
- an action held as its context turns on doesn't count as pressed (e.g. the Start that opened a menu)

Raw keys: `InputKeyDown / InputKeyPressed / InputKeyReleased(const InputState&, SDL_Scancode)`.

Engine plumbing: `InputInit`, `InputBeginFrame`, `InputProcessEvent`, `InputEndFrame`, `InputLoadActionMap`, `InputAddDefaultActions`.

## Configuration Macros

The main compile-time defaults live in `engine_configuration.h`.

Important macros:

- `ENGINE_PROJECT_NAME`
- `ENGINE_CONTENT_DIRECTORY`
- `ENGINE_PAK_ENCRYPTION_KEY_HEX`
- `ENGINE_STARTUP_MAP`
- `ENGINE_ALLOW_DEBUG_MODE` (set by the CMake option `KES_DEBUG_TOOLS`)
- `ENGINE_DEFAULT_PLAYER_CLASS_NAME`

These act as engine defaults and can be overridden at compile time.

## Current API Shape

The runtime API is intentionally small and practical. It is not a stable SDK yet. Expect signatures and responsibilities to move as the engine grows.

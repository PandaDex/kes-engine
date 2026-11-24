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

### Map loading and loading screens

`EngineLoadMap(app, "maps/next.fmapc")` queues a load, and so do `GameContext::LoadMap` and map script loads made from entity code. A queued load:

1. calls `app.onMapLoadBegin(app, mapName)`: show a loading screen here
2. draws one more frame, so the loading screen is on screen while the load blocks
3. loads the map, then uploads its meshes, textures and pipelines to the GPU (`IRenderer::PreloadAssets`), so the first frames of the level don't stall on uploads
4. calls `app.onMapLoaded(app, mapName, loaded)`

The startup map calls both hooks from `EngineStart`, without the extra frame. It is `app.startupMap` if the game set it before `EngineStart`, else the `StartupMap` of the game's `.kproject` (baked in by `kes_add_game`), else `ENGINE_STARTUP_MAP`. Textures used by both the old and the new level stay on the GPU; the rest are released. `World::LoadMap` called directly from game code still loads at once (the GPU upload then happens before the next frame).

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
3. `<user config dir>/Config/GameSettings.ini`: the player's settings, from `SDL_GetPrefPath(Company, Name)` of the `.kproject` (`%APPDATA%\<Company>\<Name>\` on Windows)

Bumping `GameSettings::kSettingsVersion` resets old player files to defaults.

Custom settings, option 1, key/value pairs stored in `[CustomSettings]`:

```cpp
settings->SetCustomFloat("FieldOfView", 0.8f);
float volume = settings->GetCustomFloat("FieldOfView", 1.0f);
```

Custom settings, option 2, a subclass (like overriding `UGameUserSettings`):

```cpp
class MySettings : public GameSettings
{
public:
    float fieldOfView = 90.0f;
protected:
    void SetToDefaults() override { GameSettings::SetToDefaults(); fieldOfView = 90.0f; }
    void LoadCustomSettings(const IniFile& ini) override { fieldOfView = ini.GetFloat("MySettings", "FieldOfView", fieldOfView); }
    void SaveCustomSettings(IniFile& ini) const override { ini.SetFloat("MySettings", "FieldOfView", fieldOfView); }
};

EngineSetGameSettingsClass<MySettings>(app);   // before EngineStart
GetGameSettings<MySettings>()->fieldOfView = 100.0f;
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

A first-person player with a 1.8 m collision capsule (`Position` is its center; the camera sits `eyeOffset`, 0.7 m, above it). Its capsule is kinematic, so it pushes dynamic bodies. `SetMovementMode` picks one of:

- `Walking` (default): gravity, jumping, running, stairs and steps up to `stepHeight` (0.4 m), slopes up to about 45 degrees, sticking to the ground when walking down steps. Simulated in `FixedUpdate`; the camera is interpolated between steps. Jumps get a short input buffer and a grace period after walking off a ledge. Falling 200 m below the spawn point puts the player back at it.
- `Flying`: free flight that collides and slides along walls (the old default).
- `Noclip`: free flight through everything.

In debug builds F3 cycles the modes. Input actions from `config/default.imap`:

| Action | Type | Default bindings |
|---|---|---|
| `InputAction_Move` | vector2 | WASD, left stick (analog: partial tilt = partial speed) |
| `InputAction_Jump` | boolean | Space, A |
| `InputAction_Sprint` | boolean | Left Shift, left stick click |
| `InputAction_MoveUp` / `InputAction_MoveDown` | boolean | Space / Left Ctrl, RB / LB (flying) |
| `InputAction_Look` | vector2 | mouse (deltas: an angle per frame) |
| `InputAction_LookStick` | vector2 | right stick (a turn rate, squared response curve) |

Look speed is scaled by `GameSettings` (`MouseSensitivity`, `GamepadLookSensitivity`, `bInvertLookY`). Tuning: `SetMoveSpeed`, `SetSprintSpeed`, `SetFlySpeed`, `SetJumpHeight`, `SetGravity`, `SetMouseLookScale`, `SetStickLookSpeed`; state: `IsGrounded`, `GetVelocity`, `GetEyePosition`. Subclasses can reuse `StepWalking`, `UpdateFlying`, `MoveWithCollision` and `UpdateLook`, and change the protected tuning fields.

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
- `void QueueMapLoad(const std::string& mapName);` loads at the end of the next `Update` (safe from inside entity code). Under the engine, queued loads are deferred (`SetDeferMapLoads`) and run by `EngineTick` with a loading frame first.
- `uint64_t GetLevelGeneration() const;` counts successful loads.
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
| `PostProcessVolumeEntity` | Enable, Disable, Toggle, SetWeight(float) | |
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
- post processing: `GetPostProcessDefaults()`, layers (`AddPostProcessLayer`, `FindPostProcessLayer`, `RemovePostProcessLayer`, `SubmitPostProcessLayer`), the resolved `GetPostProcess()` and `ResetExposureAdaptation()`. See [rendering.md](rendering.md#post-processing).

This is a runtime scene representation, not a full authored scene graph editor API.

## `AudioSystem`

`AudioSystem` plays `.ksound` clips on volume buses, with positional sound and streamed music.

Important functions:

- `AudioVoiceId Play2D(const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false, AudioBus bus = AudioBus::Effects);`
- `AudioVoiceId Play3D(const Vector3& location, float attenuation, const char* soundFile, float volume = 1.0f, float pitch = 1.0f, bool loop = false, AudioBus bus = AudioBus::Effects);`
- `AudioVoiceId PlayUi(const char* soundFile, float volume = 1.0f);` keeps playing while paused (Ui bus)
- `AudioVoiceId PlayMusic(const char* soundFile, float volume = 1.0f, bool loop = true, float fadeSeconds = 1.0f);` one track at a time, crossfaded; keeps playing while paused. `StopMusic(fadeSeconds)`.
- `void StopVoice(AudioVoiceId id, float fadeSeconds = 0.0f);`, `void FadeVoice(AudioVoiceId id, float gain, float seconds);`
- `bool IsVoicePlaying(AudioVoiceId id) const;`, `UpdateVoice3D`, `SetVoicePitch`, `SetVoiceVolume`
- `SetMasterVolume(float)`, `SetBusVolume(AudioBus, float)`: `Effects`, `Music`, `Ambient`, `Voice`, `Ui`. `GameSettings` saves these for the player (`MasterVolume`, `<Bus>Volume`) and applies them live.
- `void SetPaused(bool paused);` holds every voice except UI sounds and music (voices keep their position)
- `void StopAll();`, `bool HasOutput() const;`, `size_t GetVoiceCount() const;`

Current behavior:

- resident clips are decoded and converted to the mixer format (float stereo, 48 kHz) on load. QOA clips of 8 s or more (`kStreamMinSeconds`) and all music stay compressed in memory and are decoded while they play (about a tenth of the memory).
- mixes on SDL's audio thread (an `SDL_AudioStream` bound to the default playback device), with interpolated resampling for pitch and other sample rates. Gain changes ramp across each buffer, so moving 3D sounds and volume changes don't click.
- 3D gain is `1 / (1 + distance * attenuation)`, faded to exactly 0 where that falls under 1% (`ComputeDistanceGain`), plus stereo pan from the listener
- up to `kMaxVoices` (256) voices exist at once; the loudest `kMaxMixedVoices` (64) are mixed each buffer and the rest keep their place silently. When the list is full, a new sound replaces the quietest non-looping voice if it is louder; otherwise the play call returns 0.
- without an output device (headless machine, no sound card), it runs silent instead of failing startup

`AudioSourceEntity` takes a `Bus` property (`Effects`, `Music`, `Ambient`, `Voice`, `Ui`).

## `UiSystem`

HTML/CSS user interface (RmlUi). The engine owns it in `EngineApp::ui` and drives its lifecycle; game code loads documents and reacts to them:

- `Rml::ElementDocument* LoadDocument(const std::string& path, bool show = true);`
- `void CloseDocument(Rml::ElementDocument* document);`
- `void RegisterAction(const std::string& name, UiActionCallback callback);` for `onclick="name args"`
- `Rml::Context* GetContext() const;` for data models and the rest of the Rml API

Full guide: [ui-system.md](ui-system.md).

## On-screen debug messages

`on_screen_debug.h`: messages in the top-left corner, newest on top, like Unreal's `GEngine->AddOnScreenDebugMessage`. Debug builds only (`ENGINE_ALLOW_DEBUG_MODE`); in shipping builds the calls do nothing.

```cpp
AddOnScreenDebugMessage(-1, 5.0f, DebugMessageType::Warning, "Door has no target");
AddOnScreenDebugMessagef(42, 0.0f, DebugMessageType::Info, "Speed %.1f m/s", speed);   // every frame
```

- `key`: -1 adds a new line; any other value updates the line with that key in place (for values printed every frame)
- `seconds`: how long it stays; 0 shows it for one frame
- `DebugMessageType`: `Info` (blue), `Warning` (orange), `Error` (red); see `references/onScreenDebugMsg.html`
- `RemoveOnScreenDebugMessage(key)`, `ClearOnScreenDebugMessages()`; safe to call from any thread
- drawn by the UI system as its own document, over other documents, ignoring the mouse; at most 40 lines. The example game posts one of each kind on F2.

## `InputState`

`InputState` (`input.h`) holds the raw input snapshot and the **input actions** loaded from the project's `config/default.imap`.

Raw state: `keys` / `previousKeys`, `mouseButtons` / `previousMouseButtons`, `mouseDeltaX/Y`, `mouseWheel`, and the connected gamepads (`gamepad` is the one used last; all are opened). `InputGetActiveDevice` says whether keyboard/mouse or a gamepad was used last, `InputGetGamepadType` which kind of pad.

Bindable keys: `Keyboard_<SDL scancode name>` (`Keyboard_Left Shift`), `Mouse_Left`, `Mouse_Right`, `Mouse_Middle`, `Mouse_Button4`, `Mouse_Button5`, `Mouse_WheelUp`, `Mouse_WheelDown`, `Mouse_2D_Axis`, and the `Gamepad_*` names (`Gamepad_Face_Bottom`, `Gamepad_LeftTrigger`, `Gamepad_LeftAxis_2D`, ...).

Actions (preferred over raw keys, since players can rebind them and they cover keyboard and gamepad):

- `bool InputActionDown / InputActionPressed / InputActionReleased(const InputState&, const char* action);`
- `Vector2 InputActionValue2D(...)`, `Vector3 InputActionValue3D(...)`
- game actions read as idle in `InputMode::UIOnly`. UI actions (`UI_*`, `"Context": "UI"`) only read while the UI navigates (see [ui-system.md](ui-system.md#navigation-keyboard-and-gamepad)).
- an action held as its context turns on doesn't count as pressed (e.g. the Start that opened a menu)

Raw keys: `InputKeyDown / InputKeyPressed / InputKeyReleased(const InputState&, SDL_Scancode)`.

Rebinding (for a controls menu). Only bindings with `"bPlayerAssignable": "true"` in the map can change, to a key of the same device and shape (a button for a button, a stick for a stick); their modifiers stay:

- `InputGetBindings(input, action)` lists an action's bindings (index, current and default key, device, assignable)
- `InputBeginKeyCapture(input, device)`, then `InputGetCapturedKey(input, key)` once the player presses something; the press doesn't reach actions
- `InputRebind(input, action, index, key, &error)`, `InputResetBinding`, `InputResetAllBindings`, `InputFindActionsUsingKey` (to warn about or swap duplicates)
- `InputSaveBindingOverrides(input)` writes the player's changes to `<user data>/Config/InputBindings.ini`; the engine loads them at startup (`InputLoadBindingOverrides`). A non-`none` `AssignableKey` in the map replaces the default key.

Button prompts: `InputGetKeyDisplayName(input, key)` and `InputGetActionDisplayName(input, action, device)` give text like `E`, `Right Mouse`, `A` / `Cross`, `LB` / `L1`; keyboard names follow the player's layout.

Vibration: `InputRumble(input, low, high, seconds)`, `InputRumbleTriggers`, `InputStopRumble`, on the active pad, off when `GameSettings::SetGamepadVibrationEnabled(false)`.

Engine plumbing: `InputInit`, `InputBeginFrame`, `InputProcessEvent`, `InputEndFrame`, `InputLoadActionMap`, `InputAddDefaultActions`.

## Configuration Macros

The main compile-time defaults live in `engine_configuration.h`.

Important macros:

- `ENGINE_CONTENT_DIRECTORY`
- `ENGINE_PAK_ENCRYPTION_KEY_HEX`
- `ENGINE_STARTUP_MAP` (fallback; the `.kproject` `StartupMap` and `app.startupMap` take precedence)
- `ENGINE_ALLOW_DEBUG_MODE` (set by the CMake option `KES_DEBUG_TOOLS`)
- `ENGINE_DEFAULT_PLAYER_CLASS_NAME`

These act as engine defaults and can be overridden at compile time.

## Linking

The engine is a shared library by default (`engine.dll` / `libengine.so`, CMake option `KES_ENGINE_SHARED`). RmlUi and Tracy are shared alongside it so game code and the engine use one instance of each. `bundle_runtime_libs()` copies them next to the executable and packages ship them. `-DKES_ENGINE_SHARED=OFF` builds the old static library.

On Windows all engine functions are exported automatically (`WINDOWS_EXPORT_ALL_SYMBOLS`), but data is not: a global variable or static data member declared in an engine header can't be used from game code. Expose a function instead.

## Current API Shape

The runtime API is intentionally small and practical. It is not a stable SDK yet. Expect signatures and responsibilities to move as the engine grows.

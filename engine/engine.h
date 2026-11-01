#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include "camera.h"
#include "game_settings.h"
#include "render_backend.h"
#include "entity.h"
#include "input.h"
#include "shared.h"
#include "window.h"
#include "world.h"
#include "platform/platform.h"

// How the mouse is used, same idea as Unreal's SetInputMode*:
//   GameOnly   cursor hidden and locked to the window (relative mouse), game gets all input
//   GameAndUI  cursor visible and free, game still gets keyboard/gamepad/buttons, no mouse look
//   UIOnly     cursor visible, game input actions read as idle (menus)
// The cursor is always released while the window is unfocused or minimized.
enum class InputMode
{
    GameOnly,
    GameAndUI,
    UIOnly,
};

struct EngineApp
{
    AppConfig config = {};
    AppWindow window = {};
    InputState input = {};
    std::unique_ptr<IRenderer> renderer;   // active backend, created in EngineStart
    Camera camera = {};
    SceneState sceneState = {};
    Platform* platform = nullptr;
    bool platformInitialized = false;
    World world = {};
    bool running = false;
    unsigned long long prevProcessCpu = 0;
    unsigned long long prevCounter = 0;
    double performanceFrequency = 0.0;
    float fixedDt = 1.0f / 50.0f;
    float fixedAccumulator = 0.0f;
    bool sdlInitialized = false;
    bool worldInitialized = false;

    std::unique_ptr<GameSettings> settings;
    std::function<std::unique_ptr<GameSettings>()> settingsFactory;   // EngineSetGameSettingsClass

    InputMode inputMode = InputMode::GameOnly;
    bool showMouseCursor = true;      // for GameAndUI / UIOnly
    IntPoint rendererSize = {};       // last backbuffer size handed to the renderer
    unsigned long long frameDeadline = 0;   // frame limiter, performance counter ticks
};

bool EngineStart(EngineApp& app);
bool EngineTick(EngineApp& app);
void EngineShutdown(EngineApp& app);
void EngineRegisterEntityClass(EngineApp& app, const std::string& className, EntityFactoryFunction factory);
void EngineSetPlayerClass(EngineApp& app, const std::string& className);

// Stores the process command line (see command_line.h). Call before EngineStart.
void EngineSetCommandLine(int argc, const char* const* argv);

// Mouse / cursor
void EngineSetInputMode(EngineApp& app, InputMode mode);
InputMode EngineGetInputMode(const EngineApp& app);
void EngineSetShowMouseCursor(EngineApp& app, bool show);

// Window helpers
void EngineSetWindowTitle(EngineApp& app, const char* title);
// Windowed <-> preferred fullscreen mode (also bound to Alt+Enter). Saved to settings.
void EngineToggleFullscreen(EngineApp& app);

GameSettings& EngineGetGameSettings(EngineApp& app);

// Use a game specific GameSettings subclass. Call before EngineStart.
template<typename T>
void EngineSetGameSettingsClass(EngineApp& app)
{
    static_assert(std::is_base_of_v<GameSettings, T>, "Settings class must derive from GameSettings");
    app.settingsFactory = []() -> std::unique_ptr<GameSettings> { return std::make_unique<T>(); };
}

template<typename T>
void EngineRegisterEntityClass(EngineApp& app, const std::string& className)
{
    static_assert(std::is_base_of_v<Entity, T>, "Registered entity class must derive from Entity");
    static_assert(std::is_default_constructible_v<T>, "Registered entity class must be default constructible");

    EngineRegisterEntityClass(
        app,
        className,
        [](EntityManager& manager, GameContext& context) -> Entity*
        {
            (void)context;
            return manager.CreateEntity<T>();
        });
}

template<typename T>
void EngineRegisterPlayerClass(EngineApp& app, const std::string& className)
{
    static_assert(std::is_base_of_v<Entity, T>, "Registered player class must derive from Entity");
    static_assert(std::is_constructible_v<T, Camera&, InputState&>,
                  "Registered player class must be constructible from Camera& and InputState&");

    EngineRegisterEntityClass(
        app,
        className,
        [](EntityManager& manager, GameContext& context) -> Entity*
        {
            if (!context.mainCamera || !context.input)
                return nullptr;

            return manager.CreateEntity<T>(*context.mainCamera, *context.input);
        });
    EngineSetPlayerClass(app, className);
}

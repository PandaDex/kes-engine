#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include "camera.h"
#include "render_backend.h"
#include "entity.h"
#include "input.h"
#include "shared.h"
#include "window.h"
#include "world.h"
#include "platform/platform.h"

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
};

bool EngineStart(EngineApp& app);
bool EngineTick(EngineApp& app);
void EngineShutdown(EngineApp& app);
void EngineRegisterEntityClass(EngineApp& app, const std::string& className, EntityFactoryFunction factory);
void EngineSetPlayerClass(EngineApp& app, const std::string& className);

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

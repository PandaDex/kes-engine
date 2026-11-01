#pragma once
#include <memory>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "shared.h"
#include "input.h"

struct Camera;
class Scene;
class AssetSystem;

// Debug/scene toggles shared by every renderer backend.
struct SceneState
{
    float ambient[3] = {0.25f, 0.25f, 0.25f};

    bool wireframe = false;
    bool vertexSnap = true;
    bool animDebug = false;
};

// Abstract renderer. One instance is created by the engine for the backend
// that was selected (and successfully initialized) at startup.
class IRenderer
{
public:
    virtual ~IRenderer() = default;

    virtual RendererBackend GetBackend() const = 0;

    virtual bool Init(SDL_Window *window, const AppConfig &config) = 0;
    virtual void Shutdown() = 0;

    // Backbuffer size in pixels (window drawable size).
    virtual bool Resize(int width, int height) = 0;

    // Internal render resolution (the low-res offscreen target that is scaled
    // up to the window). Recreates the offscreen targets when it changes.
    virtual bool SetRenderResolution(int width, int height) = 0;

    // Vsync on: present waits for vertical blank. Off: present immediately
    // (tearing allowed where the platform supports it).
    virtual void SetVSync(bool enabled) = 0;
    virtual bool IsVSyncEnabled() const = 0;

    virtual void RenderScene(const SceneState &scene, const Camera &camera, const Scene &sceneObj, const AssetSystem &assets) = 0;
    virtual void BeginBackbufferOverlay() = 0;
    virtual void Present() = 0;
};

// --- Backend information

const char *RendererBackendName(RendererBackend backend);

// Parses "vulkan", "vk", "d3d11", "dx11", "auto" (case-insensitive).
bool ParseRendererBackend(const char *text, RendererBackend &outBackend);

// Backends compiled into this build for the current platform, in priority order.
const std::vector<RendererBackend> &GetAvailableRendererBackends();
bool IsRendererBackendAvailable(RendererBackend backend);

// False when the platform build forces a single backend (only one compiled in,
// or KES_ALLOW_RENDERER_SELECTION=OFF). Requests are then ignored.
bool IsRendererSelectionAllowed();

// SDL window flags a backend needs on its window (e.g. SDL_WINDOW_VULKAN).
SDL_WindowFlags GetRendererWindowFlags(RendererBackend backend);

// --- Selection ---------

// The command line ("-renderer=vulkan", "--renderer d3d11", see
// command_line.h) takes precedence over the environment and AppConfig.

// Ordered list of backends to try at startup. The first entry is the one the
// user/game asked for (if it is available and selection is allowed), followed
// by the remaining backends in platform priority order as fallbacks.
std::vector<RendererBackend> BuildRendererCandidateList(RendererBackend configured);

std::unique_ptr<IRenderer> CreateRenderer(RendererBackend backend);

// Backend independent debug toggles (F1 wireframe, F2 vertex snap, F3 anim debug).
void RendererUpdateScene(SceneState &scene, const InputState &input);

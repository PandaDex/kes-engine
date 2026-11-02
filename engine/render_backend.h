#pragma once
#include <cstdint>
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
    // Ambient environment when no sky is active (a uniform environment:
    // diffuse and reflections). With a sky, ambient light comes from the sky.
    float ambient[3] = {0.25f, 0.25f, 0.25f};
    float exposure = 1.0f;   // HDR exposure before tonemapping

    bool wireframe = false;
    bool animDebug = false;
};

enum class ShadowQuality
{
    Off = 0,
    Low = 1,      // 2 sun cascades at 1024, 6 local shadow slices at 512
    Medium = 2,   // 3 cascades at 2048, 8 local slices at 512
    High = 3,     // 4 cascades at 2048, 12 local slices at 1024
};

// Player-facing quality options (GameSettings), applied with
// IRenderer::SetGraphicsOptions. Unsupported values are clamped by the backend.
struct GraphicsOptions
{
    int msaaSamples = 4;   // 1, 2, 4 or 8
    ShadowQuality shadowQuality = ShadowQuality::High;
    int anisotropy = 8;    // 1-16
    float shadowDistance = 60.0f;   // meters covered by the sun cascades

    bool operator==(const GraphicsOptions &other) const
    {
        return msaaSamples == other.msaaSamples && shadowQuality == other.shadowQuality &&
               anisotropy == other.anisotropy && shadowDistance == other.shadowDistance;
    }
    bool operator!=(const GraphicsOptions &other) const { return !(*this == other); }
};

// 2D UI geometry for one frame, built by the UI system (ui_system.h) and
// drawn by the renderer on top of the backbuffer. Positions are backbuffer
// pixels with the origin top-left; colors and textures use premultiplied alpha.
struct UiVertex
{
    float x = 0.0f;
    float y = 0.0f;
    uint8_t color[4] = {255, 255, 255, 255};   // RGBA8, premultiplied
    float u = 0.0f;
    float v = 0.0f;
};
static_assert(sizeof(UiVertex) == 20, "UiVertex must match the ui.slang vertex input");

// 0 = no texture (draws with a white texture)
using UiTextureHandle = uint64_t;

struct UiScissorRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct UiDrawCommand
{
    UiTextureHandle texture = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bool scissorEnabled = false;
    UiScissorRect scissor = {};   // backbuffer pixels, used when scissorEnabled
};

struct UiDrawList
{
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;    // absolute indices into vertices
    std::vector<UiDrawCommand> commands;

    void Clear()
    {
        vertices.clear();
        indices.clear();
        commands.clear();
    }
    bool Empty() const { return commands.empty(); }
};

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

    // MSAA, shadow quality, texture filtering. May recreate render targets.
    virtual void SetGraphicsOptions(const GraphicsOptions &options) = 0;
    virtual GraphicsOptions GetGraphicsOptions() const = 0;

    virtual void RenderScene(const SceneState &scene, const Camera &camera, const Scene &sceneObj, const AssetSystem &assets) = 0;
    virtual void BeginBackbufferOverlay() = 0;

    // UI textures (fonts, images). Pixels are tightly packed RGBA8 with
    // premultiplied alpha. Safe to call at any time; destruction is deferred
    // until the GPU no longer uses the texture.
    virtual UiTextureHandle CreateUiTexture(int width, int height, const uint8_t *rgbaPixels) = 0;
    virtual void DestroyUiTexture(UiTextureHandle texture) = 0;

    // Draws the UI over the full backbuffer (not the letterboxed scene
    // viewport). Call between BeginBackbufferOverlay and Present.
    virtual void RenderUi(const UiDrawList &drawList) = 0;

    virtual void Present() = 0;
};

const char *RendererBackendName(RendererBackend backend);

// Parses "vulkan", "vk", "d3d11", "dx11", "auto"
bool ParseRendererBackend(const char *text, RendererBackend &outBackend);

// Backends compiled into this build for the current platform, in priority order.
const std::vector<RendererBackend> &GetAvailableRendererBackends();
bool IsRendererBackendAvailable(RendererBackend backend);

// False when the platform build forces a single backend (only one compiled in,
// or KES_ALLOW_RENDERER_SELECTION=OFF). Requests are then ignored.
bool IsRendererSelectionAllowed();

// SDL window flags a backend needs on its window (e.g. SDL_WINDOW_VULKAN).
SDL_WindowFlags GetRendererWindowFlags(RendererBackend backend);

std::vector<RendererBackend> BuildRendererCandidateList(RendererBackend configured);

std::unique_ptr<IRenderer> CreateRenderer(RendererBackend backend);
void RendererUpdateScene(SceneState &scene, const InputState &input);

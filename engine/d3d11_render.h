#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "shared.h"
#include "input.h"
#include "camera.h"
#include "mesh.h"
#include "kmodel.h"
#include "material.h"
#include "render_backend.h"
#include "render_common.h"
#include "profiler.h"

#if KES_PROFILE_GPU
namespace tracy { class D3D11Ctx; }
#endif

// Direct3D 11 renderer backend (Windows only). Internal to the engine: game
// code talks to the renderer through IRenderer (render_backend.h).
//
// Frame: shadow depth passes (sun cascades + local lights) -> HDR scene pass
// (RGBA16F, MSAA) -> resolve -> tonemap to the backbuffer -> UI.

class Scene;
class AssetSystem;
struct SceneObject;

namespace kes::d3d11
{

struct DrawItem
{
    const SceneObject* object = nullptr;
    const MaterialData* material = nullptr;
    float distanceSq = 0.0f;
};

struct GPUMesh
{
    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* indexBuffer = nullptr;
    UINT indexCount = 0;
    Vector3 boundsCenter = {};   // local-space bounding sphere
    float boundsRadius = 0.0f;
};

struct GPUModelMeshKey
{
    ModelHandle model = 0;
    uint32_t meshIndex = 0;

    bool operator==(const GPUModelMeshKey& other) const
    {
        return model == other.model && meshIndex == other.meshIndex;
    }
};

struct GPUModelMeshKeyHasher
{
    size_t operator()(const GPUModelMeshKey& key) const
    {
        return (static_cast<size_t>(key.model) << 32) ^ static_cast<size_t>(key.meshIndex);
    }
};

struct GPUShaderProgram
{
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11InputLayout* inputLayout = nullptr;
};

struct GPUTexture
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
};

struct GPUModelRenderCache
{
    std::vector<float> meshBindWorldMatrices;
};

// Depth texture array with one depth view per slice (shadow maps).
struct GPUShadowArray
{
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    std::vector<ID3D11DepthStencilView*> sliceViews;
    int resolution = 0;
};

struct Renderer
{
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;

    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    // HDR scene targets at the internal render resolution. With MSAA the
    // scene renders into the multisampled pair and resolves into sceneColor.
    ID3D11Texture2D* sceneColorMS = nullptr;
    ID3D11RenderTargetView* sceneColorMSRTV = nullptr;
    ID3D11Texture2D* sceneColor = nullptr;
    ID3D11RenderTargetView* sceneColorRTV = nullptr;
    ID3D11ShaderResourceView* sceneColorSRV = nullptr;
    ID3D11Texture2D* depthTexture = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int msaaSamples = 1;   // in effect (options clamped to device support)

    ID3D11VertexShader* meshVS = nullptr;
    ID3D11PixelShader* meshPS = nullptr;
    ID3D11InputLayout* meshLayout = nullptr;

    ID3D11VertexShader* shadowVS = nullptr;
    ID3D11PixelShader* shadowPS = nullptr;

    ID3D11VertexShader* skyVS = nullptr;
    ID3D11PixelShader* skyPS = nullptr;

    ID3D11VertexShader* tonemapVS = nullptr;
    ID3D11PixelShader* tonemapPS = nullptr;
    ID3D11Buffer* tonemapCB = nullptr;

    // UI pass (ui.slang): premultiplied alpha, scissor per draw.
    ID3D11VertexShader* uiVS = nullptr;
    ID3D11PixelShader* uiPS = nullptr;
    ID3D11InputLayout* uiLayout = nullptr;
    ID3D11Buffer* uiCB = nullptr;
    ID3D11Buffer* uiVertexBuffer = nullptr;
    ID3D11Buffer* uiIndexBuffer = nullptr;
    UINT uiVertexCapacity = 0;   // in vertices
    UINT uiIndexCapacity = 0;    // in indices
    ID3D11BlendState* uiBlendState = nullptr;
    ID3D11RasterizerState* uiRS = nullptr;   // no cull, scissor enabled
    std::unordered_map<UiTextureHandle, GPUTexture> uiTextures;
    UiTextureHandle nextUiTextureHandle = 1;

    ID3D11Buffer* sceneCB = nullptr;
    ID3D11Buffer* skyCB = nullptr;
    ID3D11Buffer* modelCB = nullptr;
    ID3D11Buffer* materialCB = nullptr;

    ID3D11RasterizerState* solidRS = nullptr;
    ID3D11RasterizerState* solidNoCullRS = nullptr;
    ID3D11RasterizerState* wireRS = nullptr;
    ID3D11RasterizerState* wireNoCullRS = nullptr;
    ID3D11RasterizerState* shadowRS = nullptr;   // no cull, slope-scaled depth bias
    ID3D11BlendState* opaqueBlendState = nullptr;
    ID3D11BlendState* alphaBlendState = nullptr;
    ID3D11DepthStencilState* depthWriteState = nullptr;
    ID3D11DepthStencilState* depthReadState = nullptr;

    ID3D11SamplerState* materialSampler = nullptr;      // anisotropic, wrap (s0-s4)
    ID3D11SamplerState* linearClampSampler = nullptr;   // environment, tonemap, UI (s5)
    ID3D11SamplerState* shadowSampler = nullptr;        // comparison PCF (s6)
    GPUTexture whiteTexture = {};

    // Shadows and image-based ambient
    GPUShadowArray cascadeShadows;
    GPUShadowArray localShadows;
    GPUTexture environmentMap = {};   // RGBA16F cube with mips
    render::EnvironmentMaps environment;
    render::FrameSetup frame;
    float exposure = 1.0f;

    GraphicsOptions options = {};

    std::unordered_map<MeshHandle, GPUMesh> gpuMeshes;
    std::unordered_map<GPUModelMeshKey, GPUMesh, GPUModelMeshKeyHasher> gpuModelMeshes;
    std::unordered_map<ShaderHandle, GPUShaderProgram> gpuShaders;
    std::unordered_map<std::string, GPUTexture> gpuTextures;   // key: usage + resolved path
    std::unordered_map<ModelHandle, GPUModelRenderCache> gpuModelRenderCaches;
    uint64_t assetGeneration = 0;

    int windowWidth = 0;
    int windowHeight = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    bool preservePresentationAspect = true;
    bool preferIntegerPresentationScale = true;

    bool vsync = true;
    bool tearingSupported = false;   // DXGI_FEATURE_PRESENT_ALLOW_TEARING

#if KES_PROFILE_GPU
    tracy::D3D11Ctx* gpuProfiler = nullptr;   // Tracy GPU timestamp context
#endif
    UINT swapChainFlags = 0;         // creation flags, reused by ResizeBuffers

    // Per-frame scratch, kept to reuse its storage.
    std::vector<DrawItem> opaqueDrawItems;
    std::vector<DrawItem> transparentDrawItems;
};

bool RendererInit(Renderer& renderer, SDL_Window* window, const AppConfig& config);
void RendererShutdown(Renderer& renderer);

bool RendererResize(Renderer& renderer, int width, int height);
bool RendererSetRenderResolution(Renderer& renderer, int width, int height);
void RendererSetVSync(Renderer& renderer, bool enabled);
void RendererSetGraphicsOptions(Renderer& renderer, const GraphicsOptions& options);
void RendererRenderScene(Renderer& renderer, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets);
void RendererBeginBackbufferOverlay(Renderer& renderer);
UiTextureHandle RendererCreateUiTexture(Renderer& renderer, int width, int height, const uint8_t* rgbaPixels);
void RendererDestroyUiTexture(Renderer& renderer, UiTextureHandle texture);
void RendererRenderUi(Renderer& renderer, const UiDrawList& drawList);
void RendererPresent(Renderer& renderer);
void RendererRender(Renderer& renderer, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets);

} // namespace kes::d3d11

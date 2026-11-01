#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <SDL3/SDL.h>
#include <vector>
#include <unordered_map>
#include "shared.h"
#include "input.h"
#include "camera.h"
#include "mesh.h"
#include "kmodel.h"
#include "material.h"
#include "render_backend.h"

// Direct3D 11 renderer backend (Windows only). Internal to the engine: game
// code talks to the renderer through IRenderer (render_backend.h).

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

struct Renderer
{
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;

    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    ID3D11Texture2D* depthTexture = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;

    ID3D11Texture2D* offscreenColor = nullptr;
    ID3D11RenderTargetView* offscreenRTV = nullptr;
    ID3D11ShaderResourceView* offscreenSRV = nullptr;

    ID3D11VertexShader* meshVS = nullptr;
    ID3D11PixelShader* meshPS = nullptr;
    ID3D11InputLayout* meshLayout = nullptr;

    ID3D11VertexShader* skyVS = nullptr;
    ID3D11PixelShader* skyPS = nullptr;

    ID3D11VertexShader* blitVS = nullptr;
    ID3D11PixelShader* blitPS = nullptr;

    ID3D11Buffer* sceneCB = nullptr;
    ID3D11Buffer* skyCB = nullptr;
    ID3D11Buffer* modelCB = nullptr;
    ID3D11Buffer* materialCB = nullptr;

    ID3D11RasterizerState* solidRS = nullptr;
    ID3D11RasterizerState* solidNoCullRS = nullptr;
    ID3D11RasterizerState* wireRS = nullptr;
    ID3D11RasterizerState* wireNoCullRS = nullptr;
    ID3D11BlendState* opaqueBlendState = nullptr;
    ID3D11BlendState* alphaBlendState = nullptr;
    ID3D11DepthStencilState* depthWriteState = nullptr;
    ID3D11DepthStencilState* depthReadState = nullptr;
    ID3D11SamplerState* pointSampler = nullptr;
    GPUTexture whiteTexture = {};

    std::unordered_map<MeshHandle, GPUMesh> gpuMeshes;
    std::unordered_map<GPUModelMeshKey, GPUMesh, GPUModelMeshKeyHasher> gpuModelMeshes;
    std::unordered_map<ShaderHandle, GPUShaderProgram> gpuShaders;
    std::unordered_map<std::string, GPUTexture> gpuTextures;
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
void RendererRenderScene(Renderer& renderer, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets);
void RendererBeginBackbufferOverlay(Renderer& renderer);
void RendererPresent(Renderer& renderer);
void RendererRender(Renderer& renderer, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets);

} // namespace kes::d3d11

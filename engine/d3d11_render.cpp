#include "d3d11_render.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <dxgi1_5.h>
#include "file_paths.h"
#include "animation_pose.h"
#include "scene.h"
#include "asset_system.h"
#include "engine_configuration.h"
#include "texture_file.h"
#include "logger.h"
#include "pak_file_system.h"
#include "shader_binary.h"

#if KES_PROFILE_GPU
#include <tracy/TracyD3D11.hpp>
// GPU timestamp zone on the immediate context (shows in Tracy's GPU lane).
#define KES_D3D11_GPU_ZONE(r, name) TracyD3D11Zone((r).gpuProfiler, name)
#else
#define KES_D3D11_GPU_ZONE(r, name) ((void)0)
#endif

namespace kes::d3d11
{

using render::Matrix4;

static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

struct RenderVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
    uint16_t boneIndices[4];
    float boneWeights[4];
};

template<typename T>
static void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

static void DestroyGPUTexture(GPUTexture& texture)
{
    SafeRelease(texture.srv);
    SafeRelease(texture.texture);
}

static void DestroyShadowArray(GPUShadowArray& shadows)
{
    for (ID3D11DepthStencilView*& view : shadows.sliceViews)
        SafeRelease(view);
    shadows.sliceViews.clear();
    SafeRelease(shadows.srv);
    SafeRelease(shadows.texture);
    shadows.resolution = 0;
}

static void DestroyRendererAssetCaches(Renderer& r);
static const GPUModelRenderCache* GetOrBuildModelRenderCache(Renderer& r, ModelHandle handle, const AssetSystem& assets);

// Shaders are authored once in Slang (engine/shaders/*.slang) and compiled to
// DXBC at build time (cmake/KesShaders.cmake). Nothing is compiled at runtime.

static const D3D11_INPUT_ELEMENT_DESC kMeshInputLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

// ui.slang UiVertexInput, matches UiVertex (render_backend.h).
static const D3D11_INPUT_ELEMENT_DESC kUiInputLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

// Creates a VS/PS pair from precompiled DXBC. When outLayout is non-null the
// input layout (mesh layout by default) is created against the vertex
// shader's signature.
static bool CreateShaderPairFromBinaries(
    Renderer& r,
    const std::string& vsSource,
    const std::string& psSource,
    ID3D11VertexShader** outVS,
    ID3D11PixelShader** outPS,
    ID3D11InputLayout** outLayout,
    const D3D11_INPUT_ELEMENT_DESC* layoutElements = kMeshInputLayout,
    UINT layoutElementCount = ARRAYSIZE(kMeshInputLayout))
{
    std::vector<uint8_t> vsBytes;
    std::vector<uint8_t> psBytes;
    std::string error;
    if (!LoadCompiledShader(vsSource, ShaderStage::Vertex, RendererBackend::D3D11, vsBytes, &error) ||
        !LoadCompiledShader(psSource, ShaderStage::Pixel, RendererBackend::D3D11, psBytes, &error))
    {
        EngineLogf("%s", error.c_str());
        return false;
    }

    HRESULT hr = r.device->CreateVertexShader(vsBytes.data(), vsBytes.size(), nullptr, outVS);
    if (FAILED(hr))
    {
        EngineLogf("CreateVertexShader failed for %s: 0x%08X", vsSource.c_str(), static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreatePixelShader(psBytes.data(), psBytes.size(), nullptr, outPS);
    if (FAILED(hr))
    {
        EngineLogf("CreatePixelShader failed for %s: 0x%08X", psSource.c_str(), static_cast<unsigned>(hr));
        SafeRelease(*outVS);
        return false;
    }

    if (outLayout)
    {
        hr = r.device->CreateInputLayout(layoutElements, layoutElementCount, vsBytes.data(), vsBytes.size(), outLayout);
        if (FAILED(hr))
        {
            EngineLogf("CreateInputLayout failed for %s: 0x%08X", vsSource.c_str(), static_cast<unsigned>(hr));
            SafeRelease(*outPS);
            SafeRelease(*outVS);
            return false;
        }
    }

    return true;
}

// Render targets

static void DestroySceneTargets(Renderer& r)
{
    SafeRelease(r.sceneColorSRV);
    SafeRelease(r.sceneColorRTV);
    SafeRelease(r.sceneColor);
    SafeRelease(r.sceneColorMSRTV);
    SafeRelease(r.sceneColorMS);
    SafeRelease(r.dsv);
    SafeRelease(r.depthTexture);
}

static void DestroySwapchainTargets(Renderer& r)
{
    SafeRelease(r.rtv);
    SafeRelease(r.backBuffer);
}

// Largest supported sample count <= requested (1, 2, 4, 8).
static int ChooseMsaaSamples(Renderer& r, int requested)
{
    for (int samples = std::clamp(requested, 1, 8); samples > 1; samples /= 2)
    {
        UINT colorQuality = 0, depthQuality = 0;
        if (SUCCEEDED(r.device->CheckMultisampleQualityLevels(kSceneColorFormat, samples, &colorQuality)) &&
            SUCCEEDED(r.device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, samples, &depthQuality)) &&
            colorQuality > 0 && depthQuality > 0)
            return samples;
    }
    return 1;
}

static bool CreateSceneTargets(Renderer& r)
{
    r.msaaSamples = ChooseMsaaSamples(r, r.options.msaaSamples);
    const UINT width = static_cast<UINT>(std::max(r.renderWidth, 1));
    const UINT height = static_cast<UINT>(std::max(r.renderHeight, 1));

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kSceneColorFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = r.device->CreateTexture2D(&desc, nullptr, &r.sceneColor);
    if (SUCCEEDED(hr))
        hr = r.device->CreateRenderTargetView(r.sceneColor, nullptr, &r.sceneColorRTV);
    if (SUCCEEDED(hr))
        hr = r.device->CreateShaderResourceView(r.sceneColor, nullptr, &r.sceneColorSRV);
    if (FAILED(hr))
    {
        EngineLogf("Create HDR scene target failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    if (r.msaaSamples > 1)
    {
        D3D11_TEXTURE2D_DESC msDesc = desc;
        msDesc.SampleDesc.Count = static_cast<UINT>(r.msaaSamples);
        msDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = r.device->CreateTexture2D(&msDesc, nullptr, &r.sceneColorMS);
        if (SUCCEEDED(hr))
            hr = r.device->CreateRenderTargetView(r.sceneColorMS, nullptr, &r.sceneColorMSRTV);
        if (FAILED(hr))
        {
            EngineLogf("Create MSAA scene target failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC depthDesc = desc;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = static_cast<UINT>(r.msaaSamples);
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = r.device->CreateTexture2D(&depthDesc, nullptr, &r.depthTexture);
    if (SUCCEEDED(hr))
        hr = r.device->CreateDepthStencilView(r.depthTexture, nullptr, &r.dsv);
    if (FAILED(hr))
    {
        EngineLogf("Create depth target failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

static bool CreateSwapchainTargets(Renderer& r)
{
    HRESULT hr = r.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&r.backBuffer);
    if (FAILED(hr))
    {
        EngineLogf("GetBuffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreateRenderTargetView(r.backBuffer, nullptr, &r.rtv);
    if (FAILED(hr))
    {
        EngineLogf("CreateRenderTargetView failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

static bool CreateShadowArray(Renderer& r, GPUShadowArray& shadows, int slices, int resolution, const char* name)
{
    DestroyShadowArray(shadows);
    if (slices <= 0 || resolution <= 0)
        return true;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(resolution);
    desc.Height = static_cast<UINT>(resolution);
    desc.MipLevels = 1;
    desc.ArraySize = static_cast<UINT>(slices);
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = r.device->CreateTexture2D(&desc, nullptr, &shadows.texture);
    if (FAILED(hr))
    {
        EngineLogf("Create %s shadow maps failed (%d x %d): 0x%08X", name, slices, resolution, static_cast<unsigned>(hr));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = static_cast<UINT>(slices);
    hr = r.device->CreateShaderResourceView(shadows.texture, &srvDesc, &shadows.srv);
    if (FAILED(hr))
    {
        DestroyShadowArray(shadows);
        return false;
    }

    shadows.sliceViews.resize(static_cast<size_t>(slices), nullptr);
    for (int slice = 0; slice < slices; ++slice)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(slice);
        dsvDesc.Texture2DArray.ArraySize = 1;
        hr = r.device->CreateDepthStencilView(shadows.texture, &dsvDesc, &shadows.sliceViews[slice]);
        if (FAILED(hr))
        {
            DestroyShadowArray(shadows);
            return false;
        }
    }
    shadows.resolution = resolution;
    return true;
}

static bool CreateShadowTargets(Renderer& r)
{
    const render::ShadowConfig config = render::GetShadowConfig(r.options.shadowQuality);
    return CreateShadowArray(r, r.cascadeShadows, config.cascadeCount, config.cascadeResolution, "cascade") &&
           CreateShadowArray(r, r.localShadows, config.localSlices, config.localResolution, "local");
}

static bool CreateDeviceAndSwapChain(Renderer& r, SDL_Window* window)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    UINT deviceFlags = 0;
#ifdef _DEBUG
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
        requestedLevels, ARRAYSIZE(requestedLevels), D3D11_SDK_VERSION, &r.device, &featureLevel, &r.context);

    if (FAILED(hr) && (deviceFlags & D3D11_CREATE_DEVICE_DEBUG) != 0)
    {
        EngineLog("D3D11 debug layer unavailable; retrying without it.");
        deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
            requestedLevels, ARRAYSIZE(requestedLevels), D3D11_SDK_VERSION, &r.device, &featureLevel, &r.context);
    }

    if (FAILED(hr))
    {
        EngineLogf("D3D11CreateDevice failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    if (featureLevel < D3D_FEATURE_LEVEL_11_0)
    {
        EngineLogf("D3D11 device only supports feature level 0x%X; shader model 5.0 (feature level 11_0) is required.",
                   static_cast<unsigned>(featureLevel));
        return false;
    }

    // Factory that created the device.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    hr = r.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (SUCCEEDED(hr))
        hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr))
        hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);
    if (FAILED(hr) || !factory)
    {
        EngineLogf("Failed to get the DXGI factory: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // Tearing (vsync off without the compositor capping the frame rate) needs
    // DXGI 1.5 and a flip-model swapchain.
    IDXGIFactory5* factory5 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5))))
    {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
            r.tearingSupported = allowTearing == TRUE;
        SafeRelease(factory5);
    }

    // Flip model (Windows 10+): lower latency, independent flip in borderless
    // fullscreen, required for tearing.
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(r.windowWidth);
    desc.Height = static_cast<UINT>(r.windowHeight);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = r.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    bool flipModel = true;
    IDXGISwapChain1* swapChain1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(r.device, hwnd, &desc, nullptr, nullptr, &swapChain1);
    if (SUCCEEDED(hr))
    {
        r.swapChain = swapChain1;
        r.swapChainFlags = desc.Flags;
    }
    else
    {
        // Older systems: blt-model swapchain.
        EngineLogf("Flip-model swapchain unavailable (0x%08X); using the legacy swapchain.", static_cast<unsigned>(hr));
        r.tearingSupported = false;
        flipModel = false;
        DXGI_SWAP_CHAIN_DESC legacy = {};
        legacy.BufferDesc.Width = desc.Width;
        legacy.BufferDesc.Height = desc.Height;
        legacy.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        legacy.SampleDesc.Count = 1;
        legacy.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        legacy.BufferCount = 1;
        legacy.OutputWindow = hwnd;
        legacy.Windowed = TRUE;
        legacy.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(r.device, &legacy, &r.swapChain);
        r.swapChainFlags = 0;
    }

    // Window modes (Alt+Enter, fullscreen) are handled by the engine through SDL.
    if (SUCCEEDED(hr))
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    SafeRelease(factory);

    if (FAILED(hr))
    {
        EngineLogf("Swapchain creation failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    EngineLogf("D3D11 swapchain: %s, tearing %s.",
               flipModel ? "flip model" : "legacy",
               r.tearingSupported ? "supported" : "not supported");
    return true;
}

// Fixed state objects

static bool CreateRasterizerStates(Renderer& r)
{
    D3D11_RASTERIZER_DESC solid = {};
    solid.FillMode = D3D11_FILL_SOLID;
    solid.CullMode = D3D11_CULL_BACK;
    solid.DepthClipEnable = TRUE;
    solid.MultisampleEnable = TRUE;

    D3D11_RASTERIZER_DESC solidNoCull = solid;
    solidNoCull.CullMode = D3D11_CULL_NONE;
    D3D11_RASTERIZER_DESC wire = solid;
    wire.FillMode = D3D11_FILL_WIREFRAME;
    D3D11_RASTERIZER_DESC wireNoCull = wire;
    wireNoCull.CullMode = D3D11_CULL_NONE;

    // Shadows: both faces (open brushes, foliage), slope-scaled bias against acne.
    D3D11_RASTERIZER_DESC shadow = {};
    shadow.FillMode = D3D11_FILL_SOLID;
    shadow.CullMode = D3D11_CULL_NONE;
    shadow.DepthClipEnable = TRUE;
    shadow.DepthBias = 1000;
    shadow.SlopeScaledDepthBias = 2.0f;
    shadow.DepthBiasClamp = 0.01f;

    const struct { const D3D11_RASTERIZER_DESC* desc; ID3D11RasterizerState** state; } states[] = {
        { &solid, &r.solidRS }, { &solidNoCull, &r.solidNoCullRS }, { &wire, &r.wireRS },
        { &wireNoCull, &r.wireNoCullRS }, { &shadow, &r.shadowRS } };
    for (const auto& entry : states)
    {
        const HRESULT hr = r.device->CreateRasterizerState(entry.desc, entry.state);
        if (FAILED(hr))
        {
            EngineLogf("Create rasterizer state failed: 0x%08X", static_cast<unsigned>(hr));
            return false;
        }
    }
    return true;
}

static bool CreateBlendStates(Renderer& r)
{
    D3D11_BLEND_DESC opaque = {};
    opaque.RenderTarget[0].BlendEnable = FALSE;
    opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = r.device->CreateBlendState(&opaque, &r.opaqueBlendState);
    if (FAILED(hr))
    {
        EngineLogf("Create opaque blend state failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_BLEND_DESC alpha = {};
    alpha.RenderTarget[0].BlendEnable = TRUE;
    alpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    alpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    alpha.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    alpha.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    alpha.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    alpha.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    alpha.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = r.device->CreateBlendState(&alpha, &r.alphaBlendState);
    if (FAILED(hr))
    {
        EngineLogf("Create alpha blend state failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

static bool CreateDepthStates(Renderer& r)
{
    D3D11_DEPTH_STENCIL_DESC depthWrite = {};
    depthWrite.DepthEnable = TRUE;
    depthWrite.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthWrite.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

    HRESULT hr = r.device->CreateDepthStencilState(&depthWrite, &r.depthWriteState);
    if (FAILED(hr))
    {
        EngineLogf("Create depth write state failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthRead = depthWrite;
    depthRead.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    hr = r.device->CreateDepthStencilState(&depthRead, &r.depthReadState);
    if (FAILED(hr))
    {
        EngineLogf("Create depth read state failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

static bool CreateMaterialSampler(Renderer& r)
{
    SafeRelease(r.materialSampler);
    const int anisotropy = std::clamp(r.options.anisotropy, 1, 16);
    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = anisotropy > 1 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.MaxAnisotropy = static_cast<UINT>(anisotropy);
    samp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    const HRESULT hr = r.device->CreateSamplerState(&samp, &r.materialSampler);
    if (FAILED(hr))
    {
        EngineLogf("Create material sampler failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

static bool CreateSamplers(Renderer& r)
{
    if (!CreateMaterialSampler(r))
        return false;

    D3D11_SAMPLER_DESC linear = {};
    linear.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    linear.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    linear.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = r.device->CreateSamplerState(&linear, &r.linearClampSampler);
    if (FAILED(hr))
    {
        EngineLogf("Create linear sampler failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // PCF: bilinear depth comparison; outside the map counts as lit.
    D3D11_SAMPLER_DESC shadow = {};
    shadow.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadow.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    shadow.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    shadow.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    shadow.BorderColor[0] = shadow.BorderColor[1] = shadow.BorderColor[2] = shadow.BorderColor[3] = 1.0f;
    shadow.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    shadow.MaxLOD = D3D11_FLOAT32_MAX;
    hr = r.device->CreateSamplerState(&shadow, &r.shadowSampler);
    if (FAILED(hr))
    {
        EngineLogf("Create shadow sampler failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

static bool CreatePipelines(Renderer& r)
{
    return CreateShaderPairFromBinaries(r, kEngineShaderMeshDefault, kEngineShaderMeshDefault, &r.meshVS, &r.meshPS, &r.meshLayout) &&
           CreateShaderPairFromBinaries(r, kEngineShaderShadow, kEngineShaderShadow, &r.shadowVS, &r.shadowPS, nullptr) &&
           CreateShaderPairFromBinaries(r, kEngineShaderSky, kEngineShaderSky, &r.skyVS, &r.skyPS, nullptr) &&
           CreateShaderPairFromBinaries(r, kEngineShaderBlit, kEngineShaderBlit, &r.tonemapVS, &r.tonemapPS, nullptr);
}

static GPUShaderProgram* GetOrCompileShaderProgram(Renderer& r, ShaderHandle handle, const AssetSystem& assets)
{
    if (handle == 0)
        return nullptr;

    auto it = r.gpuShaders.find(handle);
    if (it != r.gpuShaders.end())
        return &it->second;

    KES_ZONE(RENDER, "Compile shader program (cache miss)");
    const ShaderData* shader = assets.GetShader(handle);
    if (!shader)
        return nullptr;

    // Stages a .shader asset doesn't provide fall back to the engine's template program.
    const std::string vs = shader->vertexShaderFile.empty() ? std::string(kEngineShaderMeshFallback) : shader->vertexShaderFile;
    const std::string ps = shader->pixelShaderFile.empty() ? std::string(kEngineShaderMeshFallback) : shader->pixelShaderFile;

    GPUShaderProgram program = {};
    if (!CreateShaderPairFromBinaries(r, vs, ps, &program.vertexShader, &program.pixelShader, &program.inputLayout))
    {
        EngineLogf("Failed to create shader program for \"%s\".", shader->name.c_str());
        return nullptr;
    }

    auto inserted = r.gpuShaders.emplace(handle, program);
    return &inserted.first->second;
}

static bool CreateUiPipeline(Renderer& r)
{
    if (!CreateShaderPairFromBinaries(r, kEngineShaderUi, kEngineShaderUi, &r.uiVS, &r.uiPS, &r.uiLayout,
                                      kUiInputLayout, ARRAYSIZE(kUiInputLayout)))
        return false;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 16;   // float4 scaleOffset
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = r.device->CreateBuffer(&cbDesc, nullptr, &r.uiCB);
    if (FAILED(hr))
    {
        EngineLogf("Create UI constant buffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = r.device->CreateBlendState(&blend, &r.uiBlendState);
    if (FAILED(hr))
    {
        EngineLogf("Create UI blend state failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = TRUE;
    hr = r.device->CreateRasterizerState(&raster, &r.uiRS);
    if (FAILED(hr))
    {
        EngineLogf("Create UI rasterizer state failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

// Dynamic buffer holding at least `required` elements of `stride` bytes.
static bool EnsureUiBuffer(Renderer& r, ID3D11Buffer*& buffer, UINT& capacity, size_t required, UINT stride, UINT bindFlags)
{
    if (buffer && capacity >= required)
        return true;

    SafeRelease(buffer);
    capacity = 0;
    UINT newCapacity = 4096;
    while (newCapacity < required)
        newCapacity *= 2;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = newCapacity * stride;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bindFlags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr = r.device->CreateBuffer(&desc, nullptr, &buffer);
    if (FAILED(hr))
    {
        EngineLogf("Create UI buffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    capacity = newCapacity;
    return true;
}

static bool UploadDynamic(Renderer& r, ID3D11Buffer* buffer, const void* data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(r.context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, data, bytes);
    r.context->Unmap(buffer, 0);
    return true;
}

static bool CreateConstantBuffers(Renderer& r)
{
    const struct { UINT size; ID3D11Buffer** buffer; bool dynamic; } buffers[] = {
        { sizeof(render::SceneCBData), &r.sceneCB, false },
        { sizeof(render::SkyCBData), &r.skyCB, false },
        // Model constants change every draw: dynamic, written with
        // MAP_WRITE_DISCARD so only the used part of the 8 KB block is touched.
        { sizeof(render::ModelCBData), &r.modelCB, true },
        { sizeof(render::MaterialCBData), &r.materialCB, false },
        { sizeof(render::TonemapCBData), &r.tonemapCB, true },
    };
    for (const auto& entry : buffers)
    {
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = entry.size;
        cb.Usage = entry.dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        cb.CPUAccessFlags = entry.dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT hr = r.device->CreateBuffer(&cb, nullptr, entry.buffer);
        if (FAILED(hr))
        {
            EngineLogf("Create constant buffer (%u bytes) failed: 0x%08X", entry.size, static_cast<unsigned>(hr));
            return false;
        }
    }
    return true;
}

static D3D11_VIEWPORT BuildPresentationViewport(const Renderer& r)
{
    D3D11_VIEWPORT vp = {};
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    if (r.windowWidth <= 0 || r.windowHeight <= 0 || r.renderWidth <= 0 || r.renderHeight <= 0)
        return vp;

    float targetWidth = static_cast<float>(r.windowWidth);
    float targetHeight = static_cast<float>(r.windowHeight);

    if (r.preservePresentationAspect)
    {
        const float scaleX = static_cast<float>(r.windowWidth) / static_cast<float>(r.renderWidth);
        const float scaleY = static_cast<float>(r.windowHeight) / static_cast<float>(r.renderHeight);
        float scale = scaleX < scaleY ? scaleX : scaleY;

        if (r.preferIntegerPresentationScale && scale >= 1.0f)
            scale = static_cast<float>(static_cast<int>(scale));

        targetWidth = static_cast<float>(r.renderWidth) * scale;
        targetHeight = static_cast<float>(r.renderHeight) * scale;
        if (targetWidth < 1.0f) targetWidth = 1.0f;
        if (targetHeight < 1.0f) targetHeight = 1.0f;
    }

    vp.Width = targetWidth;
    vp.Height = targetHeight;
    vp.TopLeftX = (static_cast<float>(r.windowWidth) - targetWidth) * 0.5f;
    vp.TopLeftY = (static_cast<float>(r.windowHeight) - targetHeight) * 0.5f;
    return vp;
}

// Textures

static bool CreateTexture2D(Renderer& r, const std::vector<render::MipLevel>& levels, DXGI_FORMAT format, GPUTexture& out)
{
    if (levels.empty())
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = levels[0].width;
    desc.Height = levels[0].height;
    desc.MipLevels = static_cast<UINT>(levels.size());
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
    for (size_t i = 0; i < levels.size(); ++i)
    {
        init[i].pSysMem = levels[i].pixels.data();
        init[i].SysMemPitch = levels[i].width * 4u;
    }
    HRESULT hr = r.device->CreateTexture2D(&desc, init.data(), &out.texture);
    if (SUCCEEDED(hr))
        hr = r.device->CreateShaderResourceView(out.texture, nullptr, &out.srv);
    if (FAILED(hr))
    {
        EngineLogf("Create texture failed (%ux%u): 0x%08X", desc.Width, desc.Height, static_cast<unsigned>(hr));
        DestroyGPUTexture(out);
        return false;
    }
    return true;
}

static bool CreateWhiteTexture(Renderer& r)
{
    const uint32_t pixel = 0xFFFFFFFFu;
    std::vector<render::MipLevel> levels(1);
    levels[0].width = levels[0].height = 1;
    levels[0].pixels.assign(reinterpret_cast<const uint8_t*>(&pixel), reinterpret_cast<const uint8_t*>(&pixel) + 4);
    return CreateTexture2D(r, levels, DXGI_FORMAT_R8G8B8A8_UNORM, r.whiteTexture);
}

// Material texture with mips, in the color space its slot needs. Failed loads
// are cached as empty so they aren't retried every frame.
static ID3D11ShaderResourceView* GetMaterialTexture(Renderer& r, const std::string& path, render::TextureUsage usage)
{
    if (path.empty())
        return nullptr;
    const std::string key = std::to_string(static_cast<int>(usage)) + "|" + path;
    auto it = r.gpuTextures.find(key);
    if (it != r.gpuTextures.end())
        return it->second.srv;

    KES_ZONE(RENDER, "Load material texture (cache miss)");
    KES_ZONE_TEXTF(RENDER, "%s", path.c_str());
    GPUTexture texture = {};
    TextureFileData file = {};
    std::string error;
    if (LoadTextureFile(path.c_str(), file, &error) && file.pixels.size() >= static_cast<size_t>(file.width) * file.height * 4u)
    {
        std::vector<render::MipLevel> levels;
        render::GenerateMips(file.pixels.data(), file.width, file.height, usage, levels);
        const DXGI_FORMAT format = usage == render::TextureUsage::Color ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        CreateTexture2D(r, levels, format, texture);
    }
    else
    {
        EngineLogf("LoadTextureFile failed for %s: %s", path.c_str(), error.c_str());
    }
    return r.gpuTextures.emplace(key, texture).first->second.srv;
}

// Binds the material's five textures (white where missing) and returns the
// Has*Texture flags for the shader.
static uint32_t BindMaterialTextures(Renderer& r, const MaterialData* material)
{
    ID3D11ShaderResourceView* views[render::kMaterialTextureCount] = {};
    uint32_t flags = 0;
    if (material)
    {
        const struct { const std::string* path; render::TextureUsage usage; uint32_t flag; } slots[render::kMaterialTextureCount] = {
            { &material->baseTexturePath, render::TextureUsage::Color, MaterialFlag_HasBaseTexture },
            { &material->normalTexturePath, render::TextureUsage::Normal, MaterialFlag_HasNormalTexture },
            { &material->metallicRoughnessTexturePath, render::TextureUsage::Linear, MaterialFlag_HasMetallicRoughnessTexture },
            { &material->occlusionTexturePath, render::TextureUsage::Linear, MaterialFlag_HasOcclusionTexture },
            { &material->emissiveTexturePath, render::TextureUsage::Color, MaterialFlag_HasEmissiveTexture },
        };
        for (int i = 0; i < render::kMaterialTextureCount; ++i)
        {
            views[i] = GetMaterialTexture(r, *slots[i].path, slots[i].usage);
            if (views[i])
                flags |= slots[i].flag;
        }
    }
    for (ID3D11ShaderResourceView*& view : views)
    {
        if (!view)
            view = r.whiteTexture.srv;
    }
    r.context->PSSetShaderResources(0, render::kMaterialTextureCount, views);
    return flags;
}

static bool UploadEnvironment(Renderer& r)
{
    DestroyGPUTexture(r.environmentMap);
    const render::EnvironmentMaps& env = r.environment;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(env.faceSize);
    desc.Height = static_cast<UINT>(env.faceSize);
    desc.MipLevels = static_cast<UINT>(env.mipCount);
    desc.ArraySize = 6;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    std::vector<D3D11_SUBRESOURCE_DATA> init(static_cast<size_t>(6 * env.mipCount));
    for (int face = 0; face < 6; ++face)
    {
        for (int mip = 0; mip < env.mipCount; ++mip)
        {
            D3D11_SUBRESOURCE_DATA& data = init[static_cast<size_t>(face * env.mipCount + mip)];
            data.pSysMem = env.faces[mip][face].data();
            data.SysMemPitch = static_cast<UINT>(std::max(env.faceSize >> mip, 1)) * 8u;
        }
    }
    HRESULT hr = r.device->CreateTexture2D(&desc, init.data(), &r.environmentMap.texture);
    if (SUCCEEDED(hr))
        hr = r.device->CreateShaderResourceView(r.environmentMap.texture, nullptr, &r.environmentMap.srv);
    if (FAILED(hr))
    {
        EngineLogf("Create environment cube failed: 0x%08X", static_cast<unsigned>(hr));
        DestroyGPUTexture(r.environmentMap);
        return false;
    }
    return true;
}

// Meshes

static bool UploadMeshBuffers(Renderer& r, const std::vector<RenderVertex>& vertices, const std::vector<uint32_t>& indices, GPUMesh& gpu)
{
    gpu.indexCount = static_cast<UINT>(indices.empty() ? vertices.size() : indices.size());
    render::ComputeBoundingSphere(vertices.data(), vertices.size(), sizeof(RenderVertex), gpu.boundsCenter, gpu.boundsRadius);

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(RenderVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = vertices.data();
    if (FAILED(r.device->CreateBuffer(&bd, &init, &gpu.vertexBuffer)))
        return false;

    if (!indices.empty())
    {
        bd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = indices.data();
        if (FAILED(r.device->CreateBuffer(&bd, &init, &gpu.indexBuffer)))
        {
            SafeRelease(gpu.vertexBuffer);
            return false;
        }
    }
    return true;
}

static GPUMesh* GetOrUploadMesh(Renderer& r, MeshHandle handle, const AssetSystem& assets)
{
    if (handle == 0) return nullptr;

    auto it = r.gpuMeshes.find(handle);
    if (it != r.gpuMeshes.end())
        return &it->second;

    KES_ZONE(RENDER, "Upload mesh (cache miss)");
    const MeshData* data = assets.GetMesh(handle);
    if (!data || data->vertices.empty())
        return nullptr;

    std::vector<RenderVertex> vertices;
    vertices.reserve(data->vertices.size());
    for (const MeshVertex& source : data->vertices)
    {
        RenderVertex vertex = {};
        vertex.px = source.px;
        vertex.py = source.py;
        vertex.pz = source.pz;
        vertex.nx = source.nx;
        vertex.ny = source.ny;
        vertex.nz = source.nz;
        vertex.u = source.u;
        vertex.v = source.v;
        vertex.r = source.r;
        vertex.g = source.g;
        vertex.b = source.b;
        vertex.a = source.a;
        vertex.boneIndices[0] = 0;
        vertex.boneWeights[0] = 1.0f;
        vertices.push_back(vertex);
    }

    GPUMesh gpu = {};
    if (!UploadMeshBuffers(r, vertices, data->indices, gpu))
        return nullptr;
    return &(r.gpuMeshes[handle] = gpu);
}

static GPUMesh* GetOrUploadModelMesh(Renderer& r, ModelHandle handle, uint32_t meshIndex, const AssetSystem& assets)
{
    if (handle == 0)
        return nullptr;

    const GPUModelMeshKey key = { handle, meshIndex };
    auto it = r.gpuModelMeshes.find(key);
    if (it != r.gpuModelMeshes.end())
        return &it->second;

    KES_ZONE(RENDER, "Upload model mesh (cache miss)");
    const KModelData* model = assets.GetModel(handle);
    if (!model || meshIndex >= model->meshes.size() || model->meshes[meshIndex].vertices.empty())
        return nullptr;

    const KModelMesh& source = model->meshes[meshIndex];
    std::vector<RenderVertex> vertices;
    vertices.reserve(source.vertices.size());
    for (const KModelVertex& value : source.vertices)
    {
        RenderVertex vertex = {};
        vertex.px = value.px;
        vertex.py = value.py;
        vertex.pz = value.pz;
        vertex.nx = value.nx;
        vertex.ny = value.ny;
        vertex.nz = value.nz;
        vertex.u = value.u;
        vertex.v = value.v;
        vertex.r = value.r;
        vertex.g = value.g;
        vertex.b = value.b;
        vertex.a = value.a;
        for (int index = 0; index < 4; ++index)
        {
            vertex.boneIndices[index] = value.boneIndices[index];
            vertex.boneWeights[index] = value.boneWeights[index];
        }
        vertices.push_back(vertex);
    }

    GPUMesh gpu = {};
    if (!UploadMeshBuffers(r, vertices, source.indices, gpu))
        return nullptr;
    return &(r.gpuModelMeshes[key] = gpu);
}

static int32_t FindModelNodeForMesh(const KModelData& model, uint32_t meshIndex)
{
    for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
    {
        if (model.nodes[nodeIndex].meshIndex == static_cast<int32_t>(meshIndex))
            return static_cast<int32_t>(nodeIndex);
    }
    return -1;
}

static void DestroyRendererAssetCaches(Renderer& r)
{
    KES_ZONE_FN(RENDER);
    for (auto& pair : r.gpuTextures)
        DestroyGPUTexture(pair.second);
    r.gpuTextures.clear();

    for (auto& pair : r.gpuMeshes)
    {
        SafeRelease(pair.second.vertexBuffer);
        SafeRelease(pair.second.indexBuffer);
    }
    r.gpuMeshes.clear();

    for (auto& pair : r.gpuModelMeshes)
    {
        SafeRelease(pair.second.vertexBuffer);
        SafeRelease(pair.second.indexBuffer);
    }
    r.gpuModelMeshes.clear();

    for (auto& pair : r.gpuShaders)
    {
        SafeRelease(pair.second.inputLayout);
        SafeRelease(pair.second.pixelShader);
        SafeRelease(pair.second.vertexShader);
    }
    r.gpuShaders.clear();

    r.gpuModelRenderCaches.clear();
}

static const GPUModelRenderCache* GetOrBuildModelRenderCache(Renderer& r, ModelHandle handle, const AssetSystem& assets)
{
    if (handle == 0)
        return nullptr;

    auto it = r.gpuModelRenderCaches.find(handle);
    if (it != r.gpuModelRenderCaches.end())
        return &it->second;

    KES_ZONE(RENDER, "Build model render cache (cache miss)");
    const KModelData* model = assets.GetModel(handle);
    if (!model)
        return nullptr;

    GPUModelRenderCache cache = {};
    cache.meshBindWorldMatrices.resize(model->meshes.size() * 16u, 0.0f);

    std::vector<AnimationTransform> bindPose;
    BuildAnimationBindPose(*model, bindPose);
    std::vector<float> bindWorldMatrices;
    BuildAnimationWorldMatrices(*model, bindPose, bindWorldMatrices);

    for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(model->meshes.size()); ++meshIndex)
    {
        const int32_t meshNodeIndex = FindModelNodeForMesh(*model, meshIndex);

        float* destination = cache.meshBindWorldMatrices.data() + static_cast<size_t>(meshIndex) * 16u;
        destination[0] = 1.0f;
        destination[5] = 1.0f;
        destination[10] = 1.0f;
        destination[15] = 1.0f;

        if (meshNodeIndex >= 0 && bindWorldMatrices.size() >= (static_cast<size_t>(meshNodeIndex) + 1u) * 16u)
            std::memcpy(destination, bindWorldMatrices.data() + static_cast<size_t>(meshNodeIndex) * 16u, sizeof(float) * 16u);
    }

    auto inserted = r.gpuModelRenderCaches.emplace(handle, std::move(cache));
    return &inserted.first->second;
}

// Init / resize / options

bool RendererInit(Renderer& r, SDL_Window* window, const AppConfig& config)
{
    r.windowWidth = config.windowWidth;
    r.windowHeight = config.windowHeight;
    r.renderWidth = config.renderWidth;
    r.renderHeight = config.renderHeight;
    r.preservePresentationAspect = config.preservePresentationAspect;
    r.preferIntegerPresentationScale = config.preferIntegerPresentationScale;
    r.vsync = config.vsync;

    if (!CreateDeviceAndSwapChain(r, window))
    {
        RendererShutdown(r);
        return false;
    }

#if KES_PROFILE_GPU
    r.gpuProfiler = TracyD3D11Context(r.device, r.context);
    TracyD3D11ContextName(r.gpuProfiler, "D3D11", 5);
#endif

    const bool ok = CreateSwapchainTargets(r) && CreateSceneTargets(r) && CreateShadowTargets(r) &&
                    CreatePipelines(r) && CreateUiPipeline(r) && CreateConstantBuffers(r) &&
                    CreateRasterizerStates(r) && CreateBlendStates(r) && CreateDepthStates(r) &&
                    CreateSamplers(r) && CreateWhiteTexture(r);
    if (!ok)
    {
        RendererShutdown(r);
        return false;
    }

    EngineLogf("Renderer initialized successfully. window=%dx%d render=%dx%d, MSAA %dx.", r.windowWidth, r.windowHeight,
               r.renderWidth, r.renderHeight, r.msaaSamples);
    return true;
}

bool RendererResize(Renderer& r, int width, int height)
{
    KES_ZONE_FN(RENDER);
    if (!r.swapChain || width <= 0 || height <= 0)
        return false;

    r.windowWidth = width;
    r.windowHeight = height;

    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    DestroySwapchainTargets(r);

    HRESULT hr = r.swapChain->ResizeBuffers(
        0,
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN,
        r.swapChainFlags);   // must match the creation flags (ALLOW_TEARING)

    if (FAILED(hr))
    {
        EngineLogf("ResizeBuffers failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return CreateSwapchainTargets(r);
}

bool RendererSetRenderResolution(Renderer& r, int width, int height)
{
    KES_ZONE_FN(RENDER);
    if (!r.swapChain || width <= 0 || height <= 0)
        return false;
    if (width == r.renderWidth && height == r.renderHeight)
        return true;

    r.renderWidth = width;
    r.renderHeight = height;
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    DestroySceneTargets(r);
    return CreateSceneTargets(r);
}

void RendererSetVSync(Renderer& r, bool enabled)
{
    r.vsync = enabled;
}

void RendererSetGraphicsOptions(Renderer& r, const GraphicsOptions& options)
{
    const GraphicsOptions previous = r.options;
    r.options = options;
    if (!r.device || previous == options)
        return;

    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* nullViews[8] = {};
    r.context->PSSetShaderResources(0, 8, nullViews);
    if (previous.msaaSamples != options.msaaSamples)
    {
        DestroySceneTargets(r);
        CreateSceneTargets(r);
    }
    if (previous.shadowQuality != options.shadowQuality)
        CreateShadowTargets(r);
    if (previous.anisotropy != options.anisotropy)
        CreateMaterialSampler(r);
    EngineLogf("Graphics options: MSAA %dx, shadows %d, anisotropy %dx.", r.msaaSamples, static_cast<int>(options.shadowQuality),
               std::clamp(options.anisotropy, 1, 16));
}

// Per-draw constants

// Writes the model constants the shader reads (matrices, skin info, used skin
// matrices) instead of the whole 8 KB block. The rest of the discarded buffer
// is undefined, which is fine: the shader only reads skin matrices < count.
static void UpdateModelConstants(Renderer& r, const Matrix4& wvp, const Matrix4& world, const float* skinMatrices, uint32_t skinMatrixCount)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(r.context->Map(r.modelCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    std::memcpy(dst + offsetof(render::ModelCBData, worldViewProj), wvp.m, sizeof(wvp.m));
    std::memcpy(dst + offsetof(render::ModelCBData, world), world.m, sizeof(world.m));
    if (skinMatrices && skinMatrixCount > 0)
        std::memcpy(dst + offsetof(render::ModelCBData, skinMatrices), skinMatrices, skinMatrixCount * 16u * sizeof(float));
    const float skinInfo[4] = { skinMatrixCount > 0 ? 1.0f : 0.0f, static_cast<float>(skinMatrixCount), 0.0f, 0.0f };
    std::memcpy(dst + offsetof(render::ModelCBData, skinInfo), skinInfo, sizeof(skinInfo));
    r.context->Unmap(r.modelCB, 0);
}

static bool IsTransparentMaterial(const MaterialData* material)
{
    return material && (material->flags & MaterialFlag_Transparent) != 0;
}

static bool IsDoubleSidedMaterial(const MaterialData* material)
{
    return material && (material->flags & MaterialFlag_DoubleSided) != 0;
}

static bool CastsShadows(const MaterialData* material)
{
    return !material || ((material->flags & MaterialFlag_CastShadows) != 0 && (material->flags & MaterialFlag_Transparent) == 0);
}

static void BuildClearColor(const Scene& sceneObj, float outColor[4])
{
    outColor[0] = 0.07f;
    outColor[1] = 0.06f;
    outColor[2] = 0.10f;
    outColor[3] = 1.0f;

    const FogSettings& fog = sceneObj.GetFog();
    if (fog.active)
    {
        outColor[0] = fog.color[0];
        outColor[1] = fog.color[1];
        outColor[2] = fog.color[2];
        return;
    }

    const SkySettings& sky = sceneObj.GetSky();
    if (!sky.active)
        return;

    for (int i = 0; i < 3; ++i)
        outColor[i] = sky.horizonColor[i] * 0.65f + sky.bottomColor[i] * 0.35f;
}

static void BindFullscreenTriangle(Renderer& r)
{
    ID3D11Buffer* noVB = nullptr;
    UINT zero = 0;
    r.context->IASetInputLayout(nullptr);
    r.context->IASetVertexBuffers(0, 1, &noVB, &zero, &zero);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

static void RenderSkyBackground(Renderer& r, const Camera& camera, const Scene& sceneObj)
{
    if (sceneObj.GetFog().active || !sceneObj.GetSky().active)
        return;

    const float aspect = static_cast<float>(r.renderWidth) / static_cast<float>(r.renderHeight);
    const render::SkyCBData skyCB = render::BuildSkyConstants(sceneObj.GetSky(), camera, aspect);
    r.context->UpdateSubresource(r.skyCB, 0, nullptr, &skyCB, 0, 0);

    const float blendFactor[4] = { 0, 0, 0, 0 };
    BindFullscreenTriangle(r);
    r.context->VSSetShader(r.skyVS, nullptr, 0);
    r.context->PSSetShader(r.skyPS, nullptr, 0);
    r.context->PSSetConstantBuffers(0, 1, &r.skyCB);
    r.context->RSSetState(r.solidNoCullRS);
    r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
    r.context->OMSetDepthStencilState(r.depthReadState, 0);
    r.context->Draw(3, 0);
}

// Redundant-state filter for consecutive draws.
struct DrawState
{
    ID3D11RasterizerState* rs = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* ib = nullptr;
    const MaterialData* material = nullptr;
    bool haveMaterial = false;
};

static void DrawGPUMesh(Renderer& r, DrawState& state, const GPUMesh& gpuMesh)
{
    if (gpuMesh.vertexBuffer != state.vb)
    {
        UINT stride = sizeof(RenderVertex);
        UINT offset = 0;
        r.context->IASetVertexBuffers(0, 1, &gpuMesh.vertexBuffer, &stride, &offset);
        state.vb = gpuMesh.vertexBuffer;
    }
    if (gpuMesh.indexBuffer)
    {
        if (gpuMesh.indexBuffer != state.ib)
        {
            r.context->IASetIndexBuffer(gpuMesh.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            state.ib = gpuMesh.indexBuffer;
        }
        r.context->DrawIndexed(gpuMesh.indexCount, 0, 0);
    }
    else
    {
        r.context->Draw(gpuMesh.indexCount, 0);
    }
}

// Draws one scene object with `viewProj` (camera or shadow view), culled
// against `frustum`. Shadow passes use the shadow program.
static void DrawSceneObject(Renderer& r, DrawState& state, const DrawItem& item, const Matrix4& viewProj,
                            const render::Frustum& frustum, const AssetSystem& assets, bool shadowPass, bool wireframe)
{
    const SceneObject& obj = *item.object;
    const KModelData* model = assets.GetModel(obj.model);
    const bool skinned = model && !model->bones.empty() && obj.skinMatrixCount > 0;

    const bool doubleSided = IsDoubleSidedMaterial(item.material);
    ID3D11RasterizerState* rs = shadowPass ? r.shadowRS
                                           : (wireframe ? (doubleSided ? r.wireNoCullRS : r.wireRS)
                                                        : (doubleSided ? r.solidNoCullRS : r.solidRS));
    ID3D11InputLayout* layout = r.meshLayout;
    ID3D11VertexShader* vs = shadowPass ? r.shadowVS : r.meshVS;
    ID3D11PixelShader* ps = shadowPass ? r.shadowPS : r.meshPS;
    if (!shadowPass && item.material && item.material->shader != 0)
    {
        if (GPUShaderProgram* custom = GetOrCompileShaderProgram(r, item.material->shader, assets))
        {
            layout = custom->inputLayout;
            vs = custom->vertexShader;
            ps = custom->pixelShader;
        }
    }
    if (rs != state.rs) { r.context->RSSetState(rs); state.rs = rs; }
    if (layout != state.layout) { r.context->IASetInputLayout(layout); state.layout = layout; }
    if (vs != state.vs) { r.context->VSSetShader(vs, nullptr, 0); state.vs = vs; }
    if (ps != state.ps) { r.context->PSSetShader(ps, nullptr, 0); state.ps = ps; }

    if (!state.haveMaterial || item.material != state.material)
    {
        const uint32_t textureFlags = BindMaterialTextures(r, item.material);
        const render::MaterialCBData cb = render::BuildMaterialConstants(item.material, textureFlags);
        r.context->UpdateSubresource(r.materialCB, 0, nullptr, &cb, 0, 0);
        state.material = item.material;
        state.haveMaterial = true;
    }

    const Matrix4 objectWorld = render::TransformToMatrix(obj.transform);
    if (skinned)
    {
        const GPUModelRenderCache* modelRenderCache = GetOrBuildModelRenderCache(r, obj.model, assets);
        if (!modelRenderCache)
            return;

        // Animated: no culling (the bind-pose bounds don't cover the animation).
        const uint32_t matrixCount = std::min<uint32_t>(obj.skinMatrixCount, SceneObject::MaxSkinMatrices);
        for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(model->meshes.size()); ++meshIndex)
        {
            GPUMesh* gpuMesh = GetOrUploadModelMesh(r, obj.model, meshIndex, assets);
            if (!gpuMesh)
                continue;
            Matrix4 meshLocal;
            std::memcpy(meshLocal.m, modelRenderCache->meshBindWorldMatrices.data() + static_cast<size_t>(meshIndex) * 16u, sizeof(meshLocal.m));
            const Matrix4 world = render::MatrixMultiply(meshLocal, objectWorld);
            UpdateModelConstants(r, render::MatrixMultiply(world, viewProj), world, obj.skinMatrices, matrixCount);
            DrawGPUMesh(r, state, *gpuMesh);
        }
        return;
    }

    GPUMesh* gpuMesh = GetOrUploadMesh(r, obj.mesh, assets);
    if (!gpuMesh)
        return;
    Vector3 center;
    float radius = 0.0f;
    render::TransformSphere(obj.transform, gpuMesh->boundsCenter, gpuMesh->boundsRadius, center, radius);
    if (!render::SphereInFrustum(frustum, center, radius))
        return;
    UpdateModelConstants(r, render::MatrixMultiply(objectWorld, viewProj), objectWorld, nullptr, 0);
    DrawGPUMesh(r, state, *gpuMesh);
}

static void RenderShadowMaps(Renderer& r, const AssetSystem& assets)
{
    if (r.frame.shadowViews.empty())
        return;
    KES_ZONE(RENDER, "Shadow maps");
    KES_D3D11_GPU_ZONE(r, "Shadows");

    // The arrays are written now and sampled by the scene pass.
    ID3D11ShaderResourceView* nullViews[3] = {};
    r.context->PSSetShaderResources(5, 3, nullViews);

    const float blendFactor[4] = { 0, 0, 0, 0 };
    r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
    r.context->OMSetDepthStencilState(r.depthWriteState, 0);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetConstantBuffers(0, 1, &r.sceneCB);
    r.context->VSSetConstantBuffers(1, 1, &r.modelCB);
    r.context->VSSetConstantBuffers(2, 1, &r.materialCB);
    r.context->PSSetConstantBuffers(2, 1, &r.materialCB);
    ID3D11SamplerState* materialSamplers[render::kMaterialTextureCount] = { r.materialSampler, r.materialSampler, r.materialSampler, r.materialSampler, r.materialSampler };
    r.context->PSSetSamplers(0, render::kMaterialTextureCount, materialSamplers);

    for (const render::ShadowView& view : r.frame.shadowViews)
    {
        GPUShadowArray& target = view.cascade ? r.cascadeShadows : r.localShadows;
        if (view.slice >= static_cast<int>(target.sliceViews.size()))
            continue;
        ID3D11DepthStencilView* dsv = target.sliceViews[static_cast<size_t>(view.slice)];
        r.context->OMSetRenderTargets(0, nullptr, dsv);
        r.context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp = {};
        vp.Width = vp.Height = static_cast<float>(target.resolution);
        vp.MaxDepth = 1.0f;
        r.context->RSSetViewports(1, &vp);

        // The light's frustum (far plane = range for local lights) culls casters.
        const render::Frustum frustum = render::ExtractFrustum(view.viewProj);
        DrawState state;
        for (const DrawItem& item : r.opaqueDrawItems)
        {
            if (CastsShadows(item.material))
                DrawSceneObject(r, state, item, view.viewProj, frustum, assets, true, false);
        }
    }
}

void RendererRenderScene(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    if (!r.sceneColor || !r.dsv)
        return;
    if (r.assetGeneration != assets.GetGeneration())
    {
        DestroyRendererAssetCaches(r);
        r.assetGeneration = assets.GetGeneration();
    }

    KES_D3D11_GPU_ZONE(r, "Scene");
    const float aspect = static_cast<float>(r.renderWidth) / static_cast<float>(r.renderHeight);

    // Ambient environment, rebuilt when the sky (or flat ambient) changes.
    const uint64_t environmentKey = render::EnvironmentKey(scene, sceneObj);
    if (environmentKey != r.environment.key || !r.environmentMap.srv)
    {
        KES_ZONE(RENDER, "Build environment");
        render::BuildEnvironment(scene, sceneObj, r.environment);
        UploadEnvironment(r);
    }

    render::BuildFrameSetup(r.frame, scene, camera, sceneObj, aspect, r.options);
    render::ApplyEnvironment(r.environment, r.frame.scene);
    if (!r.cascadeShadows.srv)
    {
        r.frame.scene.shadowParams[3] = 0.0f;
        r.frame.shadowViews.clear();
    }
    r.exposure = std::max(scene.exposure, 0.0f);
    r.context->UpdateSubresource(r.sceneCB, 0, nullptr, &r.frame.scene, 0, 0);

    // Draw lists (shared by shadow and scene passes).
    std::vector<DrawItem>& opaqueItems = r.opaqueDrawItems;
    std::vector<DrawItem>& transparentItems = r.transparentDrawItems;
    opaqueItems.clear();
    transparentItems.clear();
    const Vector3 cameraPosition = CameraGetPosition(camera);
    {
        KES_ZONE(RENDER, "Build draw lists");
        for (const SceneObject& obj : sceneObj.GetObjects())
        {
            if (!obj.active)
                continue;
            DrawItem item = {};
            item.object = &obj;
            item.material = assets.GetMaterial(obj.material);
            item.distanceSq = Vector3LengthSq(obj.transform.position - cameraPosition);
            (IsTransparentMaterial(item.material) ? transparentItems : opaqueItems).push_back(item);
        }
        // Opaque: group by material, near-to-far inside a group (early depth reject).
        std::sort(opaqueItems.begin(), opaqueItems.end(), [](const DrawItem& a, const DrawItem& b)
                  { return a.material != b.material ? a.material < b.material : a.distanceSq < b.distanceSq; });
        // Transparent: back-to-front for correct blending.
        std::sort(transparentItems.begin(), transparentItems.end(), [](const DrawItem& a, const DrawItem& b)
                  { return a.distanceSq > b.distanceSq; });
        KES_PROFILE_PLOT("Opaque draw items", static_cast<int64_t>(opaqueItems.size()));
        KES_PROFILE_PLOT("Transparent draw items", static_cast<int64_t>(transparentItems.size()));
    }

    RenderShadowMaps(r, assets);

    // HDR scene pass
    ID3D11RenderTargetView* sceneRTV = r.msaaSamples > 1 ? r.sceneColorMSRTV : r.sceneColorRTV;
    r.context->OMSetRenderTargets(1, &sceneRTV, r.dsv);
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<FLOAT>(r.renderWidth);
    vp.Height = static_cast<FLOAT>(r.renderHeight);
    vp.MaxDepth = 1.0f;
    r.context->RSSetViewports(1, &vp);

    float clearColor[4] = {};
    BuildClearColor(sceneObj, clearColor);
    r.context->ClearRenderTargetView(sceneRTV, clearColor);
    r.context->ClearDepthStencilView(r.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    RenderSkyBackground(r, camera, sceneObj);

    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetConstantBuffers(0, 1, &r.sceneCB);
    r.context->VSSetConstantBuffers(1, 1, &r.modelCB);
    r.context->VSSetConstantBuffers(2, 1, &r.materialCB);
    r.context->PSSetConstantBuffers(0, 1, &r.sceneCB);
    r.context->PSSetConstantBuffers(2, 1, &r.materialCB);
    ID3D11SamplerState* samplers[7] = { r.materialSampler, r.materialSampler, r.materialSampler, r.materialSampler,
                                        r.materialSampler, r.linearClampSampler, r.shadowSampler };
    r.context->PSSetSamplers(0, 7, samplers);
    ID3D11ShaderResourceView* globals[3] = { r.environmentMap.srv, r.cascadeShadows.srv, r.localShadows.srv };
    r.context->PSSetShaderResources(5, 3, globals);

    const float blendFactor[4] = { 0, 0, 0, 0 };
    DrawState state;
    {
        KES_ZONE(RENDER, "Draw opaque");
        KES_D3D11_GPU_ZONE(r, "Opaque");
        r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
        r.context->OMSetDepthStencilState(r.depthWriteState, 0);
        for (const DrawItem& item : opaqueItems)
            DrawSceneObject(r, state, item, r.frame.viewProj, r.frame.frustum, assets, false, scene.wireframe);
    }
    {
        KES_ZONE(RENDER, "Draw transparent");
        KES_D3D11_GPU_ZONE(r, "Transparent");
        r.context->OMSetBlendState(r.alphaBlendState, blendFactor, 0xFFFFFFFF);
        r.context->OMSetDepthStencilState(r.depthReadState, 0);
        for (const DrawItem& item : transparentItems)
            DrawSceneObject(r, state, item, r.frame.viewProj, r.frame.frustum, assets, false, scene.wireframe);
    }

    if (r.msaaSamples > 1)
    {
        KES_D3D11_GPU_ZONE(r, "MSAA resolve");
        r.context->ResolveSubresource(r.sceneColor, 0, r.sceneColorMS, 0, kSceneColorFormat);
    }
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
}

void RendererBeginBackbufferOverlay(Renderer& r)
{
    KES_D3D11_GPU_ZONE(r, "Tonemap");
    const float black[4] = { 0, 0, 0, 1 };
    r.context->OMSetRenderTargets(1, &r.rtv, nullptr);
    r.context->ClearRenderTargetView(r.rtv, black);
    if (!r.sceneColorSRV)
        return;

    D3D11_VIEWPORT vp = BuildPresentationViewport(r);
    r.context->RSSetViewports(1, &vp);

    render::TonemapCBData tonemap = {};
    tonemap.params[0] = r.exposure;
    tonemap.params[1] = 1.0f / 255.0f;
    UploadDynamic(r, r.tonemapCB, &tonemap, sizeof(tonemap));

    BindFullscreenTriangle(r);
    r.context->VSSetShader(r.tonemapVS, nullptr, 0);
    r.context->PSSetShader(r.tonemapPS, nullptr, 0);
    r.context->PSSetConstantBuffers(0, 1, &r.tonemapCB);
    r.context->PSSetShaderResources(0, 1, &r.sceneColorSRV);
    r.context->PSSetSamplers(0, 1, &r.linearClampSampler);
    r.context->RSSetState(r.solidNoCullRS);
    const float blendFactor[4] = { 0, 0, 0, 0 };
    r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
    r.context->OMSetDepthStencilState(nullptr, 0);
    r.context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    r.context->PSSetShaderResources(0, 1, &nullSRV);
}

// UI

UiTextureHandle RendererCreateUiTexture(Renderer& r, int width, int height, const uint8_t* rgbaPixels)
{
    KES_ZONE_FN(RENDER);
    if (!r.device || width <= 0 || height <= 0 || !rgbaPixels)
        return 0;

    std::vector<render::MipLevel> levels(1);
    levels[0].width = static_cast<uint32_t>(width);
    levels[0].height = static_cast<uint32_t>(height);
    levels[0].pixels.assign(rgbaPixels, rgbaPixels + static_cast<size_t>(width) * height * 4u);
    GPUTexture texture = {};
    if (!CreateTexture2D(r, levels, DXGI_FORMAT_R8G8B8A8_UNORM, texture))
        return 0;

    const UiTextureHandle handle = r.nextUiTextureHandle++;
    r.uiTextures.emplace(handle, texture);
    return handle;
}

void RendererDestroyUiTexture(Renderer& r, UiTextureHandle handle)
{
    // The driver keeps resources alive while queued GPU work still uses them.
    auto it = r.uiTextures.find(handle);
    if (it == r.uiTextures.end())
        return;
    DestroyGPUTexture(it->second);
    r.uiTextures.erase(it);
}

void RendererRenderUi(Renderer& r, const UiDrawList& drawList)
{
    if (!r.rtv || drawList.Empty() || drawList.vertices.empty() || drawList.indices.empty())
        return;

    KES_ZONE_FN(RENDER);
    KES_D3D11_GPU_ZONE(r, "UI");

    if (!EnsureUiBuffer(r, r.uiVertexBuffer, r.uiVertexCapacity, drawList.vertices.size(), sizeof(UiVertex), D3D11_BIND_VERTEX_BUFFER) ||
        !EnsureUiBuffer(r, r.uiIndexBuffer, r.uiIndexCapacity, drawList.indices.size(), sizeof(uint32_t), D3D11_BIND_INDEX_BUFFER))
        return;
    if (!UploadDynamic(r, r.uiVertexBuffer, drawList.vertices.data(), drawList.vertices.size() * sizeof(UiVertex)) ||
        !UploadDynamic(r, r.uiIndexBuffer, drawList.indices.data(), drawList.indices.size() * sizeof(uint32_t)))
        return;

    // Pixels -> clip space; D3D clip Y points up, UI coordinates point down.
    const float width = static_cast<float>(r.windowWidth);
    const float height = static_cast<float>(r.windowHeight);
    const float scaleOffset[4] = { 2.0f / width, -2.0f / height, -1.0f, 1.0f };
    if (!UploadDynamic(r, r.uiCB, scaleOffset, sizeof(scaleOffset)))
        return;

    r.context->OMSetRenderTargets(1, &r.rtv, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = width;
    vp.Height = height;
    vp.MaxDepth = 1.0f;
    r.context->RSSetViewports(1, &vp);
    r.context->RSSetState(r.uiRS);
    const float blendFactor[4] = { 0, 0, 0, 0 };
    r.context->OMSetBlendState(r.uiBlendState, blendFactor, 0xFFFFFFFF);
    r.context->OMSetDepthStencilState(nullptr, 0);

    const UINT stride = sizeof(UiVertex);
    const UINT offset = 0;
    r.context->IASetInputLayout(r.uiLayout);
    r.context->IASetVertexBuffers(0, 1, &r.uiVertexBuffer, &stride, &offset);
    r.context->IASetIndexBuffer(r.uiIndexBuffer, DXGI_FORMAT_R32_UINT, 0);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.uiVS, nullptr, 0);
    r.context->VSSetConstantBuffers(0, 1, &r.uiCB);
    r.context->PSSetShader(r.uiPS, nullptr, 0);
    r.context->PSSetSamplers(0, 1, &r.linearClampSampler);

    const LONG maxX = static_cast<LONG>(r.windowWidth);
    const LONG maxY = static_cast<LONG>(r.windowHeight);
    ID3D11ShaderResourceView* boundSRV = nullptr;
    for (const UiDrawCommand& command : drawList.commands)
    {
        if (command.indexCount == 0)
            continue;

        D3D11_RECT scissor = { 0, 0, maxX, maxY };
        if (command.scissorEnabled)
        {
            scissor.left = std::clamp<LONG>(command.scissor.x, 0, maxX);
            scissor.top = std::clamp<LONG>(command.scissor.y, 0, maxY);
            scissor.right = std::clamp<LONG>(command.scissor.x + command.scissor.width, scissor.left, maxX);
            scissor.bottom = std::clamp<LONG>(command.scissor.y + command.scissor.height, scissor.top, maxY);
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
                continue;
        }
        r.context->RSSetScissorRects(1, &scissor);

        ID3D11ShaderResourceView* srv = r.whiteTexture.srv;
        if (command.texture != 0)
        {
            auto it = r.uiTextures.find(command.texture);
            if (it != r.uiTextures.end())
                srv = it->second.srv;
        }
        if (srv != boundSRV)
        {
            r.context->PSSetShaderResources(0, 1, &srv);
            boundSRV = srv;
        }

        r.context->DrawIndexed(command.indexCount, command.firstIndex, 0);
    }

    ID3D11ShaderResourceView* nullSRV = nullptr;
    r.context->PSSetShaderResources(0, 1, &nullSRV);
}

void RendererPresent(Renderer& r)
{
    // Vsync off: present immediately; ALLOW_TEARING lets flip-model swapchains
    // in windowed/borderless mode skip the compositor's vblank wait.
    const UINT syncInterval = r.vsync ? 1u : 0u;
    const UINT presentFlags = (!r.vsync && r.tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    {
        KES_ZONE(RENDER, "IDXGISwapChain::Present");
        r.swapChain->Present(syncInterval, presentFlags);
    }

#if KES_PROFILE_GPU
    {
        KES_ZONE(GPU, "Collect GPU timestamps");
        TracyD3D11Collect(r.gpuProfiler);
    }
#endif
}

void RendererRender(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    RendererRenderScene(r, scene, camera, sceneObj, assets);
    RendererBeginBackbufferOverlay(r);
    RendererPresent(r);
}

void RendererShutdown(Renderer& r)
{
    if (r.context)
        r.context->ClearState();

#if KES_PROFILE_GPU
    if (r.gpuProfiler)
    {
        TracyD3D11Destroy(r.gpuProfiler);
        r.gpuProfiler = nullptr;
    }
#endif

    for (auto& [handle, texture] : r.uiTextures)
        DestroyGPUTexture(texture);
    r.uiTextures.clear();
    SafeRelease(r.uiRS);
    SafeRelease(r.uiBlendState);
    SafeRelease(r.uiIndexBuffer);
    SafeRelease(r.uiVertexBuffer);
    r.uiIndexCapacity = 0;
    r.uiVertexCapacity = 0;
    SafeRelease(r.uiCB);
    SafeRelease(r.uiLayout);
    SafeRelease(r.uiPS);
    SafeRelease(r.uiVS);

    DestroyGPUTexture(r.whiteTexture);
    DestroyGPUTexture(r.environmentMap);
    r.environment.key = 0;
    DestroyShadowArray(r.cascadeShadows);
    DestroyShadowArray(r.localShadows);
    DestroyRendererAssetCaches(r);
    r.assetGeneration = 0;

    SafeRelease(r.materialSampler);
    SafeRelease(r.linearClampSampler);
    SafeRelease(r.shadowSampler);
    SafeRelease(r.depthReadState);
    SafeRelease(r.depthWriteState);
    SafeRelease(r.alphaBlendState);
    SafeRelease(r.opaqueBlendState);
    SafeRelease(r.shadowRS);
    SafeRelease(r.wireNoCullRS);
    SafeRelease(r.wireRS);
    SafeRelease(r.solidNoCullRS);
    SafeRelease(r.solidRS);

    SafeRelease(r.sceneCB);
    SafeRelease(r.skyCB);
    SafeRelease(r.modelCB);
    SafeRelease(r.materialCB);
    SafeRelease(r.tonemapCB);

    SafeRelease(r.tonemapPS);
    SafeRelease(r.tonemapVS);
    SafeRelease(r.skyPS);
    SafeRelease(r.skyVS);
    SafeRelease(r.shadowPS);
    SafeRelease(r.shadowVS);
    SafeRelease(r.meshLayout);
    SafeRelease(r.meshPS);
    SafeRelease(r.meshVS);

    DestroySceneTargets(r);
    DestroySwapchainTargets(r);

    SafeRelease(r.swapChain);
    SafeRelease(r.context);
    SafeRelease(r.device);
}

} // namespace kes::d3d11

namespace
{
    class D3D11Renderer final : public IRenderer
    {
    public:
        ~D3D11Renderer() override { Shutdown(); }

        RendererBackend GetBackend() const override { return RendererBackend::D3D11; }

        bool Init(SDL_Window* window, const AppConfig& config) override
        {
            // RendererInit releases everything it created on failure.
            m_initialized = kes::d3d11::RendererInit(m_state, window, config);
            if (!m_initialized)
                m_state = {};
            return m_initialized;
        }

        void Shutdown() override
        {
            if (!m_initialized)
                return;
            kes::d3d11::RendererShutdown(m_state);
            m_state = {};
            m_initialized = false;
        }

        bool Resize(int width, int height) override { return kes::d3d11::RendererResize(m_state, width, height); }
        bool SetRenderResolution(int width, int height) override { return kes::d3d11::RendererSetRenderResolution(m_state, width, height); }
        void SetVSync(bool enabled) override { kes::d3d11::RendererSetVSync(m_state, enabled); }
        bool IsVSyncEnabled() const override { return m_state.vsync; }
        void SetGraphicsOptions(const GraphicsOptions& options) override { kes::d3d11::RendererSetGraphicsOptions(m_state, options); }
        GraphicsOptions GetGraphicsOptions() const override { return m_state.options; }

        void RenderScene(const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets) override
        {
            kes::d3d11::RendererRenderScene(m_state, scene, camera, sceneObj, assets);
        }

        void BeginBackbufferOverlay() override { kes::d3d11::RendererBeginBackbufferOverlay(m_state); }

        UiTextureHandle CreateUiTexture(int width, int height, const uint8_t* rgbaPixels) override
        {
            return kes::d3d11::RendererCreateUiTexture(m_state, width, height, rgbaPixels);
        }
        void DestroyUiTexture(UiTextureHandle texture) override { kes::d3d11::RendererDestroyUiTexture(m_state, texture); }
        void RenderUi(const UiDrawList& drawList) override { kes::d3d11::RendererRenderUi(m_state, drawList); }

        void Present() override { kes::d3d11::RendererPresent(m_state); }

    private:
        kes::d3d11::Renderer m_state = {};
        bool m_initialized = false;
    };
}

std::unique_ptr<IRenderer> CreateD3D11Renderer()
{
    return std::make_unique<D3D11Renderer>();
}

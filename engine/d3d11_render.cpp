#include "d3d11_render.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "file_paths.h"
#include "animation_pose.h"
#include "scene.h"
#include "asset_system.h"
#include "engine_configuration.h"
#include "texture_file.h"
#include "logger.h"
#include "pak_file_system.h"
#include "shader_binary.h"

namespace kes::d3d11
{

static float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

struct float3
{
    float x, y, z;
};

struct float4x4
{
    float m[4][4];
};

static constexpr float kCameraFovYRadians = 70.0f * 3.14159265f / 180.0f;

static float3 Normalize(const float3& v)
{
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len <= 0.00001f)
        return { 0.0f, 0.0f, 1.0f };
    return { v.x / len, v.y / len, v.z / len };
}

static float4x4 Identity()
{
    float4x4 r = {};
    r.m[0][0] = 1.0f;
    r.m[1][1] = 1.0f;
    r.m[2][2] = 1.0f;
    r.m[3][3] = 1.0f;
    return r;
}

static float4x4 Multiply(const float4x4& a, const float4x4& b)
{
    float4x4 r = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            r.m[row][col] =
                a.m[row][0] * b.m[0][col] +
                a.m[row][1] * b.m[1][col] +
                a.m[row][2] * b.m[2][col] +
                a.m[row][3] * b.m[3][col];
        }
    }
    return r;
}

static float4x4 RotationX(float a)
{
    float4x4 r = Identity();
    const float c = std::cos(a);
    const float s = std::sin(a);

    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
}

static float4x4 RotationY(float a)
{
    float4x4 r = Identity();
    const float c = std::cos(a);
    const float s = std::sin(a);

    r.m[0][0] = c;
    r.m[0][2] = -s;
    r.m[2][0] = s;
    r.m[2][2] = c;
    return r;
}

static float4x4 RotationZ(float a)
{
    float4x4 r = Identity();
    const float c = std::cos(a);
    const float s = std::sin(a);
    r.m[0][0] = c;
    r.m[0][1] = s;
    r.m[1][0] = -s;
    r.m[1][1] = c;
    return r;
}

static float4x4 Scale(float x, float y, float z)
{
    float4x4 r = Identity();
    r.m[0][0] = x;
    r.m[1][1] = y;
    r.m[2][2] = z;
    return r;
}

static float4x4 Translation(float x, float y, float z)
{
    float4x4 r = Identity();
    r.m[3][0] = x;
    r.m[3][1] = y;
    r.m[3][2] = z;
    return r;
}

static float4x4 PerspectiveFovLH(float fovY, float aspect, float zn, float zf)
{
    float4x4 r = {};
    const float yScale = 1.0f / std::tan(fovY * 0.5f);
    const float xScale = yScale / aspect;

    r.m[0][0] = xScale;
    r.m[1][1] = yScale;
    r.m[2][2] = zf / (zf - zn);
    r.m[2][3] = 1.0f;
    r.m[3][2] = (-zn * zf) / (zf - zn);
    return r;
}

struct MaterialCBData
{
    float baseColor[4];
    float emissiveColorAndCutoff[4];
    uint32_t flags[4];
};

struct ModelCBData
{
    float worldViewProj[16];
    float world[16];
    float skinMatrices[SceneObject::MaxSkinMatrices * 16];
    float skinInfo[4];
};

struct RenderVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
    uint16_t boneIndices[4];
    float boneWeights[4];
};

struct GPULight
{
    float position[4];  // w = range
    float color[4];     // w = intensity
    float direction[4]; // w = spotCosAngle
    uint32_t type;      // 0=Dir, 1=Point, 2=Spot
    uint32_t active;
    float padding[2];
};

struct SceneCBData
{
    float viewProj[16];
    float ambient[4];
    float snapParams[4];
    GPULight lights[8];
    float fogColor[4];
    float fogParams[4];
};

struct SkyCBData
{
    float topColor[4];
    float horizonColor[4];
    float bottomColor[4];
    float sunColor[4];
    float sunDirection[4];
    float cameraForward[4];
    float cameraRight[4];
    float cameraUp[4];
    float projectionParams[4];
    float skyParams[4];
};

static void MatrixToArrayRowMajor(const float4x4& m, float out[16])
{
    int i = 0;
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            out[i++] = m.m[row][col];
        }
    }
}

static float4x4 ArrayToMatrix(const float in[16])
{
    float4x4 m = {};
    int i = 0;
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            m.m[row][col] = in[i++];
        }
    }
    return m;
}

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

static bool CreateWhiteTexture(Renderer& r);
static bool CreateTextureFromFile(Renderer& r, const char* path, GPUTexture& outTexture);
static ID3D11ShaderResourceView* GetMaterialTextureSRV(Renderer& r, const MaterialData* material);
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

// Creates a VS/PS pair from precompiled DXBC. When outLayout is non-null the
// mesh input layout is created against the vertex shader's signature.
static bool CreateShaderPairFromBinaries(
    Renderer& r,
    const std::string& vsSource,
    const std::string& psSource,
    ID3D11VertexShader** outVS,
    ID3D11PixelShader** outPS,
    ID3D11InputLayout** outLayout)
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
        hr = r.device->CreateInputLayout(kMeshInputLayout, ARRAYSIZE(kMeshInputLayout), vsBytes.data(), vsBytes.size(), outLayout);
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

static void DestroyRenderTargets(Renderer& r)
{
    SafeRelease(r.offscreenSRV);
    SafeRelease(r.offscreenRTV);
    SafeRelease(r.offscreenColor);
    SafeRelease(r.dsv);
    SafeRelease(r.depthTexture);
    SafeRelease(r.rtv);
    SafeRelease(r.backBuffer);
}

static bool CreateRenderTargets(Renderer& r)
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

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = static_cast<UINT>(r.renderWidth);
    depthDesc.Height = static_cast<UINT>(r.renderHeight);
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = r.device->CreateTexture2D(&depthDesc, nullptr, &r.depthTexture);
    if (FAILED(hr))
    {
        EngineLogf("Create depth texture failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreateDepthStencilView(r.depthTexture, nullptr, &r.dsv);
    if (FAILED(hr))
    {
        EngineLogf("CreateDepthStencilView failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_TEXTURE2D_DESC offDesc = {};
    offDesc.Width = static_cast<UINT>(r.renderWidth);
    offDesc.Height = static_cast<UINT>(r.renderHeight);
    offDesc.MipLevels = 1;
    offDesc.ArraySize = 1;
    offDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    offDesc.SampleDesc.Count = 1;
    offDesc.Usage = D3D11_USAGE_DEFAULT;
    offDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = r.device->CreateTexture2D(&offDesc, nullptr, &r.offscreenColor);
    if (FAILED(hr))
    {
        EngineLogf("Create offscreen texture failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreateRenderTargetView(r.offscreenColor, nullptr, &r.offscreenRTV);
    if (FAILED(hr))
    {
        EngineLogf("Create offscreen RTV failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreateShaderResourceView(r.offscreenColor, nullptr, &r.offscreenSRV);
    if (FAILED(hr))
    {
        EngineLogf("Create offscreen SRV failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

static bool CreateDeviceAndSwapChain(Renderer& r, SDL_Window* window)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferDesc.Width = static_cast<UINT>(r.windowWidth);
    scd.BufferDesc.Height = static_cast<UINT>(r.windowHeight);
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

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

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        deviceFlags,
        requestedLevels,
        ARRAYSIZE(requestedLevels),
        D3D11_SDK_VERSION,
        &scd,
        &r.swapChain,
        &r.device,
        &featureLevel,
        &r.context
    );

    if (FAILED(hr) && (deviceFlags & D3D11_CREATE_DEVICE_DEBUG) != 0)
    {
        EngineLog("D3D11 debug layer unavailable; retrying without it.");
        deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags,
            requestedLevels, ARRAYSIZE(requestedLevels), D3D11_SDK_VERSION,
            &scd, &r.swapChain, &r.device, &featureLevel, &r.context);
    }

    if (FAILED(hr))
    {
        EngineLogf("D3D11CreateDeviceAndSwapChain failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    if (featureLevel < D3D_FEATURE_LEVEL_11_0)
    {
        EngineLogf("D3D11 device only supports feature level 0x%X; shader model 5.0 (feature level 11_0) is required.",
                   static_cast<unsigned>(featureLevel));
        return false;
    }

    return true;
}

static bool CreateRasterizerStates(Renderer& r)
{
    D3D11_RASTERIZER_DESC solid = {};
    solid.FillMode = D3D11_FILL_SOLID;
    solid.CullMode = D3D11_CULL_BACK;
    solid.DepthClipEnable = TRUE;

    HRESULT hr = r.device->CreateRasterizerState(&solid, &r.solidRS);
    if (FAILED(hr))
    {
        EngineLogf("Create solid rasterizer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_RASTERIZER_DESC solidNoCull = solid;
    solidNoCull.CullMode = D3D11_CULL_NONE;

    hr = r.device->CreateRasterizerState(&solidNoCull, &r.solidNoCullRS);
    if (FAILED(hr))
    {
        EngineLogf("Create solid no-cull rasterizer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_RASTERIZER_DESC wire = solid;
    wire.FillMode = D3D11_FILL_WIREFRAME;

    hr = r.device->CreateRasterizerState(&wire, &r.wireRS);
    if (FAILED(hr))
    {
        EngineLogf("Create wire rasterizer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    D3D11_RASTERIZER_DESC wireNoCull = wire;
    wireNoCull.CullMode = D3D11_CULL_NONE;

    hr = r.device->CreateRasterizerState(&wireNoCull, &r.wireNoCullRS);
    if (FAILED(hr))
    {
        EngineLogf("Create wire no-cull rasterizer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

static bool CreateSkyPipeline(Renderer& r)
{
    return CreateShaderPairFromBinaries(r, kEngineShaderSky, kEngineShaderSky, &r.skyVS, &r.skyPS, nullptr);
}

static GPUShaderProgram* GetOrCompileShaderProgram(Renderer& r, ShaderHandle handle, const AssetSystem& assets)
{
    if (handle == 0)
        return nullptr;

    auto it = r.gpuShaders.find(handle);
    if (it != r.gpuShaders.end())
        return &it->second;

    const ShaderData* shader = assets.GetShader(handle);
    if (!shader)
        return nullptr;

    // Stages a .shader asset doesn't provide fall back to the engine's fog-capable mesh program.
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

static bool CreateSampler(Renderer& r)
{
    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = r.device->CreateSamplerState(&samp, &r.pointSampler);
    if (FAILED(hr))
    {
        EngineLogf("CreateSamplerState failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

static bool CreateMeshPipeline(Renderer& r)
{
    return CreateShaderPairFromBinaries(r, kEngineShaderMeshDefault, kEngineShaderMeshDefault, &r.meshVS, &r.meshPS, &r.meshLayout);
}

static bool CreateBlitPipeline(Renderer& r)
{
    return CreateShaderPairFromBinaries(r, kEngineShaderBlit, kEngineShaderBlit, &r.blitVS, &r.blitPS, nullptr);
}

static bool CreateConstantBuffers(Renderer& r)
{
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.ByteWidth = sizeof(SceneCBData);
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    HRESULT hr = r.device->CreateBuffer(&cb, nullptr, &r.sceneCB);
    if (FAILED(hr))
    {
        EngineLogf("Create scene constant buffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    cb.ByteWidth = sizeof(SkyCBData);
    hr = r.device->CreateBuffer(&cb, nullptr, &r.skyCB);
    if (FAILED(hr))
    {
        EngineLogf("Create sky constant buffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    cb.ByteWidth = sizeof(ModelCBData);
    hr = r.device->CreateBuffer(&cb, nullptr, &r.modelCB);
    if (FAILED(hr))
    {
        EngineLogf("Create model constant buffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    cb.ByteWidth = sizeof(MaterialCBData);
    hr = r.device->CreateBuffer(&cb, nullptr, &r.materialCB);
    if (FAILED(hr))
    {
        EngineLogf("Create material constant buffer failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
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

bool RendererInit(Renderer& r, SDL_Window* window, const AppConfig& config)
{
    r.windowWidth = config.windowWidth;
    r.windowHeight = config.windowHeight;
    r.renderWidth = config.renderWidth;
    r.renderHeight = config.renderHeight;
    r.preservePresentationAspect = config.preservePresentationAspect;
    r.preferIntegerPresentationScale = config.preferIntegerPresentationScale;

    if (!CreateDeviceAndSwapChain(r, window))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateRenderTargets(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateMeshPipeline(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateSkyPipeline(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateBlitPipeline(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateConstantBuffers(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateRasterizerStates(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateBlendStates(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateDepthStates(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateSampler(r))
    {
        RendererShutdown(r);
        return false;
    }

    if (!CreateWhiteTexture(r))
    {
        RendererShutdown(r);
        return false;
    }

    EngineLogf("Renderer initialized successfully. window=%dx%d render=%dx%d.", r.windowWidth, r.windowHeight, r.renderWidth, r.renderHeight);
    return true;
}

static bool CreateWhiteTexture(Renderer& r)
{
    const uint32_t pixel = 0xFFFFFFFFu;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = &pixel;
    init.SysMemPitch = sizeof(pixel);

    HRESULT hr = r.device->CreateTexture2D(&desc, &init, &r.whiteTexture.texture);
    if (FAILED(hr))
    {
        EngineLogf("Create white texture failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreateShaderResourceView(r.whiteTexture.texture, nullptr, &r.whiteTexture.srv);
    if (FAILED(hr))
    {
        EngineLogf("Create white texture SRV failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

static bool CreateTextureFromFile(Renderer& r, const char* path, GPUTexture& outTexture)
{
    TextureFileData textureFile = {};
    std::string error;
    if (!LoadTextureFile(path, textureFile, &error))
    {
        EngineLogf("LoadTextureFile failed for %s: %s", path ? path : "<null>", error.c_str());
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = textureFile.width;
    desc.Height = textureFile.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = (textureFile.flags & TextureFileFlag_Srgb) != 0 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = textureFile.pixels.data();
    init.SysMemPitch = textureFile.width * 4u;

    HRESULT hr = r.device->CreateTexture2D(&desc, &init, &outTexture.texture);
    if (FAILED(hr))
    {
        EngineLogf("Create texture failed for %s: 0x%08X", path ? path : "<null>", static_cast<unsigned>(hr));
        return false;
    }

    hr = r.device->CreateShaderResourceView(outTexture.texture, nullptr, &outTexture.srv);
    if (FAILED(hr))
    {
        EngineLogf("Create texture SRV failed for %s: 0x%08X", path ? path : "<null>", static_cast<unsigned>(hr));
        DestroyGPUTexture(outTexture);
        return false;
    }

    return true;
}

static ID3D11ShaderResourceView* GetMaterialTextureSRV(Renderer& r, const MaterialData* material)
{
    if (!material || material->baseTexturePath.empty())
        return r.whiteTexture.srv;

    const std::string resolvedPath = ResolveAssetPath(material->baseTexturePath.c_str());
    auto it = r.gpuTextures.find(resolvedPath);
    if (it != r.gpuTextures.end())
        return it->second.srv ? it->second.srv : r.whiteTexture.srv;

    GPUTexture texture = {};
    if (!CreateTextureFromFile(r, resolvedPath.c_str(), texture))
    {
        r.gpuTextures.emplace(resolvedPath, GPUTexture{});
        return r.whiteTexture.srv;
    }

    auto inserted = r.gpuTextures.emplace(resolvedPath, texture);
    return inserted.first->second.srv ? inserted.first->second.srv : r.whiteTexture.srv;
}

bool RendererResize(Renderer& r, int width, int height)
{
    if (!r.swapChain || width <= 0 || height <= 0)
        return false;

    r.windowWidth = width;
    r.windowHeight = height;

    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    DestroyRenderTargets(r);

    HRESULT hr = r.swapChain->ResizeBuffers(
        0,
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        DXGI_FORMAT_UNKNOWN,
        0);

    if (FAILED(hr))
    {
        EngineLogf("ResizeBuffers failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    return CreateRenderTargets(r);
}

static GPUMesh* GetOrUploadMesh(Renderer& r, MeshHandle handle, const AssetSystem& assets)
{
    if (handle == 0) return nullptr;

    auto it = r.gpuMeshes.find(handle);
    if (it != r.gpuMeshes.end())
        return &it->second;

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
    gpu.indexCount = static_cast<UINT>(data->indices.size());

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(RenderVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = vertices.data();

    HRESULT hr = r.device->CreateBuffer(&bd, &init, &gpu.vertexBuffer);
    if (FAILED(hr)) return nullptr;

    if (!data->indices.empty())
    {
        bd.ByteWidth = static_cast<UINT>(data->indices.size() * sizeof(uint32_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = data->indices.data();
        hr = r.device->CreateBuffer(&bd, &init, &gpu.indexBuffer);
        if (FAILED(hr))
        {
            gpu.vertexBuffer->Release();
            return nullptr;
        }
    }

    r.gpuMeshes[handle] = gpu;
    return &r.gpuMeshes[handle];
}

static GPUMesh* GetOrUploadModelMesh(Renderer& r, ModelHandle handle, uint32_t meshIndex, const AssetSystem& assets)
{
    if (handle == 0)
        return nullptr;

    const GPUModelMeshKey key = { handle, meshIndex };
    auto it = r.gpuModelMeshes.find(key);
    if (it != r.gpuModelMeshes.end())
        return &it->second;

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
    gpu.indexCount = static_cast<UINT>(source.indices.size());

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(RenderVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = vertices.data();

    HRESULT hr = r.device->CreateBuffer(&bd, &init, &gpu.vertexBuffer);
    if (FAILED(hr))
        return nullptr;

    if (!source.indices.empty())
    {
        bd.ByteWidth = static_cast<UINT>(source.indices.size() * sizeof(uint32_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        init.pSysMem = source.indices.data();
        hr = r.device->CreateBuffer(&bd, &init, &gpu.indexBuffer);
        if (FAILED(hr))
        {
            gpu.vertexBuffer->Release();
            return nullptr;
        }
    }

    r.gpuModelMeshes[key] = gpu;
    return &r.gpuModelMeshes[key];
}

static float4x4 TransformToMatrix(const Transform& t)
{
    float4x4 r = Scale(t.scale.x, t.scale.y, t.scale.z);
    r = Multiply(r, RotationX(t.rotation.x));
    r = Multiply(r, RotationY(t.rotation.y));
    r = Multiply(r, RotationZ(t.rotation.z));
    r = Multiply(r, Translation(t.position.x, t.position.y, t.position.z));
    return r;
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

static float3 BuildLightDirection(const Transform& t)
{
    const Vector3 direction = Vector3DirectionFromRotation(t.rotation);
    return { direction.x, direction.y, direction.z };
}

struct DrawItem
{
    const SceneObject* object = nullptr;
    const MaterialData* material = nullptr;
    float distanceSq = 0.0f;
};

static bool IsTransparentMaterial(const MaterialData* material)
{
    return material && (material->flags & MaterialFlag_Transparent) != 0;
}

static bool IsDoubleSidedMaterial(const MaterialData* material)
{
    return material && (material->flags & MaterialFlag_DoubleSided) != 0;
}

static ID3D11RasterizerState* SelectRasterizerState(Renderer& r, bool wireframe, bool doubleSided)
{
    if (wireframe)
        return doubleSided ? r.wireNoCullRS : r.wireRS;
    return doubleSided ? r.solidNoCullRS : r.solidRS;
}

static void UpdateMaterialConstants(Renderer& r, const MaterialData* material)
{
    MaterialCBData matcb = {};
    matcb.baseColor[0] = 1.0f;
    matcb.baseColor[1] = 1.0f;
    matcb.baseColor[2] = 1.0f;
    matcb.baseColor[3] = 1.0f;
    matcb.flags[0] = MaterialFlag_Lit;

    if (material)
    {
        matcb.baseColor[0] = material->baseColor[0];
        matcb.baseColor[1] = material->baseColor[1];
        matcb.baseColor[2] = material->baseColor[2];
        matcb.baseColor[3] = material->baseColor[3];
        matcb.emissiveColorAndCutoff[0] = material->emissiveColor[0];
        matcb.emissiveColorAndCutoff[1] = material->emissiveColor[1];
        matcb.emissiveColorAndCutoff[2] = material->emissiveColor[2];
        matcb.emissiveColorAndCutoff[3] = material->alphaCutoff;
        matcb.flags[0] = material->flags;
    }

    r.context->UpdateSubresource(r.materialCB, 0, nullptr, &matcb, 0, 0);
}

static void BuildSkyClearColor(const Scene& sceneObj, float outColor[4])
{
    outColor[0] = 0.07f;
    outColor[1] = 0.06f;
    outColor[2] = 0.10f;
    outColor[3] = 1.0f;

    const FogSettings& fog = sceneObj.GetFog();
    if (fog.active)
    {
        outColor[0] = Clamp01(fog.color[0]);
        outColor[1] = Clamp01(fog.color[1]);
        outColor[2] = Clamp01(fog.color[2]);
        return;
    }

    const SkySettings& sky = sceneObj.GetSky();
    if (!sky.active)
        return;

    outColor[0] = Clamp01(sky.horizonColor[0] * 0.65f + sky.bottomColor[0] * 0.35f);
    outColor[1] = Clamp01(sky.horizonColor[1] * 0.65f + sky.bottomColor[1] * 0.35f);
    outColor[2] = Clamp01(sky.horizonColor[2] * 0.65f + sky.bottomColor[2] * 0.35f);
}

static void RenderSkyBackground(Renderer& r, const Camera& camera, const Scene& sceneObj)
{
    if (sceneObj.GetFog().active)
        return;

    const SkySettings& sky = sceneObj.GetSky();
    if (!sky.active)
        return;

    const float aspect = static_cast<float>(r.renderWidth) / static_cast<float>(r.renderHeight);
    const float tanHalfFov = std::tan(kCameraFovYRadians * 0.5f);
    const float sunAngleRadians = sky.sunSize * 3.14159265f / 180.0f;
    const float sunOuterCos = std::cos(sunAngleRadians);
    const float sunInnerCos = std::cos(sunAngleRadians * 0.35f);
    const Vector3 cameraForward = CameraGetForward(camera);
    const Vector3 cameraRight = CameraGetRight(camera);
    const Vector3 cameraUp = CameraGetUp(camera);

    SkyCBData skyCB = {};
    skyCB.topColor[0] = sky.topColor[0];
    skyCB.topColor[1] = sky.topColor[1];
    skyCB.topColor[2] = sky.topColor[2];
    skyCB.topColor[3] = 1.0f;
    skyCB.horizonColor[0] = sky.horizonColor[0];
    skyCB.horizonColor[1] = sky.horizonColor[1];
    skyCB.horizonColor[2] = sky.horizonColor[2];
    skyCB.horizonColor[3] = 1.0f;
    skyCB.bottomColor[0] = sky.bottomColor[0];
    skyCB.bottomColor[1] = sky.bottomColor[1];
    skyCB.bottomColor[2] = sky.bottomColor[2];
    skyCB.bottomColor[3] = 1.0f;
    skyCB.sunColor[0] = sky.sunColor[0];
    skyCB.sunColor[1] = sky.sunColor[1];
    skyCB.sunColor[2] = sky.sunColor[2];
    skyCB.sunColor[3] = 1.0f;
    skyCB.sunDirection[0] = sky.sunDirection[0];
    skyCB.sunDirection[1] = sky.sunDirection[1];
    skyCB.sunDirection[2] = sky.sunDirection[2];
    skyCB.sunDirection[3] = 0.0f;
    skyCB.cameraForward[0] = cameraForward.x;
    skyCB.cameraForward[1] = cameraForward.y;
    skyCB.cameraForward[2] = cameraForward.z;
    skyCB.cameraForward[3] = 0.0f;
    skyCB.cameraRight[0] = cameraRight.x;
    skyCB.cameraRight[1] = cameraRight.y;
    skyCB.cameraRight[2] = cameraRight.z;
    skyCB.cameraRight[3] = 0.0f;
    skyCB.cameraUp[0] = cameraUp.x;
    skyCB.cameraUp[1] = cameraUp.y;
    skyCB.cameraUp[2] = cameraUp.z;
    skyCB.cameraUp[3] = 0.0f;
    skyCB.projectionParams[0] = tanHalfFov * aspect;
    skyCB.projectionParams[1] = tanHalfFov;
    skyCB.projectionParams[2] = sunOuterCos;
    skyCB.projectionParams[3] = sunInnerCos;
    skyCB.skyParams[0] = sky.sunIntensity;
    skyCB.skyParams[1] = sky.horizonExponent;
    skyCB.skyParams[2] = 1.0f;
    skyCB.skyParams[3] = 0.0f;
    r.context->UpdateSubresource(r.skyCB, 0, nullptr, &skyCB, 0, 0);

    ID3D11Buffer* noVB = nullptr;
    UINT zero = 0;
    const float blendFactor[4] = { 0, 0, 0, 0 };
    r.context->IASetInputLayout(nullptr);
    r.context->IASetVertexBuffers(0, 1, &noVB, &zero, &zero);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetShader(r.skyVS, nullptr, 0);
    r.context->PSSetShader(r.skyPS, nullptr, 0);
    r.context->PSSetConstantBuffers(0, 1, &r.skyCB);
    r.context->RSSetState(r.solidNoCullRS);
    r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
    r.context->OMSetDepthStencilState(r.depthReadState, 0);
    r.context->Draw(3, 0);
}

void RendererRenderScene(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    if (r.assetGeneration != assets.GetGeneration())
    {
        DestroyRendererAssetCaches(r);
        r.assetGeneration = assets.GetGeneration();
    }

    float clearColor[4] = {};
    BuildSkyClearColor(sceneObj, clearColor);

    r.context->OMSetRenderTargets(1, &r.offscreenRTV, r.dsv);

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<FLOAT>(r.renderWidth);
    vp.Height = static_cast<FLOAT>(r.renderHeight);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    r.context->RSSetViewports(1, &vp);

    r.context->ClearRenderTargetView(r.offscreenRTV, clearColor);
    r.context->ClearDepthStencilView(r.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    const float blendFactor[4] = { 0, 0, 0, 0 };
    r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
    r.context->OMSetDepthStencilState(r.depthWriteState, 0);

    RenderSkyBackground(r, camera, sceneObj);

    CameraViewData camData = {};
    const float aspect = static_cast<float>(r.renderWidth) / static_cast<float>(r.renderHeight);
    CameraBuildMatrices(camera, aspect, camData);

    const float4x4 viewProj = ArrayToMatrix(camData.viewProjection);

    SceneCBData scb = {};
    MatrixToArrayRowMajor(viewProj, scb.viewProj);
    scb.ambient[0] = scene.ambient[0];
    scb.ambient[1] = scene.ambient[1];
    scb.ambient[2] = scene.ambient[2];
    scb.ambient[3] = 1.0f;
    scb.snapParams[0] = static_cast<float>(r.renderWidth);
    scb.snapParams[1] = static_cast<float>(r.renderHeight);
    scb.snapParams[2] = scene.vertexSnap ? 1.0f : 0.0f;
    scb.snapParams[3] = 0.0f;

    const auto& sceneLights = sceneObj.GetLights();
    for (size_t i = 0; i < sceneLights.size() && i < 8; ++i)
    {
        const Light& light = sceneLights[i];
        if (!light.active) continue;

        const float3 lightDirection = BuildLightDirection(light.transform);

        scb.lights[i].position[0] = light.transform.position.x;
        scb.lights[i].position[1] = light.transform.position.y;
        scb.lights[i].position[2] = light.transform.position.z;
        scb.lights[i].position[3] = light.range;

        scb.lights[i].color[0] = light.color[0];
        scb.lights[i].color[1] = light.color[1];
        scb.lights[i].color[2] = light.color[2];
        scb.lights[i].color[3] = light.intensity;

        scb.lights[i].direction[0] = lightDirection.x;
        scb.lights[i].direction[1] = lightDirection.y;
        scb.lights[i].direction[2] = lightDirection.z;
        scb.lights[i].direction[3] = std::cos(light.spotAngle * 0.5f * 3.14159265f / 180.0f);

        scb.lights[i].type = static_cast<uint32_t>(light.type);
        scb.lights[i].active = 1;
    }

    const FogSettings& fog = sceneObj.GetFog();
    scb.fogColor[0] = fog.color[0];
    scb.fogColor[1] = fog.color[1];
    scb.fogColor[2] = fog.color[2];
    scb.fogColor[3] = 1.0f;
    scb.fogParams[0] = fog.nearDistance;
    scb.fogParams[1] = fog.farDistance;
    scb.fogParams[2] = fog.active ? 1.0f : 0.0f;
    scb.fogParams[3] = fog.stepCount;

    r.context->UpdateSubresource(r.sceneCB, 0, nullptr, &scb, 0, 0);

    r.context->IASetInputLayout(r.meshLayout);
    r.context->VSSetShader(r.meshVS, nullptr, 0);
    r.context->PSSetShader(r.meshPS, nullptr, 0);
    r.context->VSSetConstantBuffers(0, 1, &r.sceneCB);
    r.context->VSSetConstantBuffers(1, 1, &r.modelCB);
    r.context->VSSetConstantBuffers(2, 1, &r.materialCB);
    r.context->PSSetConstantBuffers(0, 1, &r.sceneCB);
    r.context->PSSetConstantBuffers(2, 1, &r.materialCB);
    r.context->PSSetSamplers(0, 1, &r.pointSampler);

    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    std::vector<DrawItem> opaqueItems;
    std::vector<DrawItem> transparentItems;
    opaqueItems.reserve(sceneObj.GetObjects().size());
    transparentItems.reserve(sceneObj.GetObjects().size());
    const Vector3 cameraPosition = { camera.x, camera.y, camera.z };

    for (const SceneObject& obj : sceneObj.GetObjects())
    {
        if (!obj.active) continue;

        const MaterialData* material = assets.GetMaterial(obj.material);
        const Vector3 delta = {
            obj.transform.position.x - cameraPosition.x,
            obj.transform.position.y - cameraPosition.y,
            obj.transform.position.z - cameraPosition.z
        };
        DrawItem item = {};
        item.object = &obj;
        item.material = material;
        item.distanceSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;

        if (IsTransparentMaterial(material))
            transparentItems.push_back(item);
        else
            opaqueItems.push_back(item);
    }

    std::sort(transparentItems.begin(), transparentItems.end(),
        [](const DrawItem& a, const DrawItem& b)
        {
            return a.distanceSq > b.distanceSq;
        });

    auto drawItems = [&](const std::vector<DrawItem>& items, bool transparent)
    {
        r.context->OMSetBlendState(transparent ? r.alphaBlendState : r.opaqueBlendState, blendFactor, 0xFFFFFFFF);
        r.context->OMSetDepthStencilState(transparent ? r.depthReadState : r.depthWriteState, 0);

        for (const DrawItem& item : items)
        {
            const SceneObject& obj = *item.object;
            const KModelData* model = assets.GetModel(obj.model);
            const bool skinned = model && !model->bones.empty() && obj.skinMatrixCount > 0;

            GPUShaderProgram* customProgram = nullptr;
            if (item.material && item.material->shader != 0)
                customProgram = GetOrCompileShaderProgram(r, item.material->shader, assets);

            r.context->RSSetState(SelectRasterizerState(r, scene.wireframe, IsDoubleSidedMaterial(item.material)));
            r.context->IASetInputLayout(customProgram ? customProgram->inputLayout : r.meshLayout);
            r.context->VSSetShader(customProgram ? customProgram->vertexShader : r.meshVS, nullptr, 0);
            r.context->PSSetShader(customProgram ? customProgram->pixelShader : r.meshPS, nullptr, 0);

            if (skinned)
            {
                const GPUModelRenderCache* modelRenderCache = GetOrBuildModelRenderCache(r, obj.model, assets);
                if (!modelRenderCache)
                    continue;

                for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(model->meshes.size()); ++meshIndex)
                {
                    GPUMesh* gpuMesh = GetOrUploadModelMesh(r, obj.model, meshIndex, assets);
                    if (!gpuMesh)
                        continue;

                    float4x4 world = TransformToMatrix(obj.transform);
                    float4x4 meshLocal = {};
                    std::memcpy(&meshLocal, modelRenderCache->meshBindWorldMatrices.data() + static_cast<size_t>(meshIndex) * 16u, sizeof(meshLocal));
                    world = Multiply(meshLocal, world);

                    const float4x4 wvp = Multiply(world, viewProj);
                    ModelCBData mcb = {};
                    MatrixToArrayRowMajor(wvp, mcb.worldViewProj);
                    MatrixToArrayRowMajor(world, mcb.world);
                    mcb.skinInfo[0] = 1.0f;
                    mcb.skinInfo[1] = static_cast<float>(obj.skinMatrixCount);
                    const uint32_t matrixCount = obj.skinMatrixCount > SceneObject::MaxSkinMatrices ? SceneObject::MaxSkinMatrices : obj.skinMatrixCount;
                    for (uint32_t index = 0; index < matrixCount * 16; ++index)
                        mcb.skinMatrices[index] = obj.skinMatrices[index];
                    r.context->UpdateSubresource(r.modelCB, 0, nullptr, &mcb, 0, 0);

                    UpdateMaterialConstants(r, item.material);
                    ID3D11ShaderResourceView* textureSrv = GetMaterialTextureSRV(r, item.material);
                    r.context->PSSetShaderResources(0, 1, &textureSrv);

                    UINT stride = sizeof(RenderVertex);
                    UINT offset = 0;
                    r.context->IASetVertexBuffers(0, 1, &gpuMesh->vertexBuffer, &stride, &offset);
                    if (gpuMesh->indexBuffer)
                    {
                        r.context->IASetIndexBuffer(gpuMesh->indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                        r.context->DrawIndexed(gpuMesh->indexCount, 0, 0);
                    }
                    else
                    {
                        r.context->Draw(gpuMesh->indexCount, 0);
                    }
                }
            }
            else
            {
                GPUMesh* gpuMesh = GetOrUploadMesh(r, obj.mesh, assets);
                if (!gpuMesh)
                    continue;

                const float4x4 world = TransformToMatrix(obj.transform);
                const float4x4 wvp = Multiply(world, viewProj);
                ModelCBData mcb = {};
                MatrixToArrayRowMajor(wvp, mcb.worldViewProj);
                MatrixToArrayRowMajor(world, mcb.world);
                mcb.skinInfo[0] = 0.0f;
                r.context->UpdateSubresource(r.modelCB, 0, nullptr, &mcb, 0, 0);

                UpdateMaterialConstants(r, item.material);
                ID3D11ShaderResourceView* textureSrv = GetMaterialTextureSRV(r, item.material);
                r.context->PSSetShaderResources(0, 1, &textureSrv);

                UINT stride = sizeof(RenderVertex);
                UINT offset = 0;
                r.context->IASetVertexBuffers(0, 1, &gpuMesh->vertexBuffer, &stride, &offset);
                if (gpuMesh->indexBuffer)
                {
                    r.context->IASetIndexBuffer(gpuMesh->indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                    r.context->DrawIndexed(gpuMesh->indexCount, 0, 0);
                }
                else
                {
                    r.context->Draw(gpuMesh->indexCount, 0);
                }
            }
        }
    };

    drawItems(opaqueItems, false);
    drawItems(transparentItems, true);
}

void RendererBeginBackbufferOverlay(Renderer& r)
{
    float black[4] = { 0,0,0,1 };
    r.context->OMSetRenderTargets(1, &r.rtv, nullptr);

    D3D11_VIEWPORT vp = BuildPresentationViewport(r);
    r.context->RSSetViewports(1, &vp);

    r.context->ClearRenderTargetView(r.rtv, black);

    ID3D11Buffer* noVB = nullptr;
    UINT zero = 0;
    r.context->IASetInputLayout(nullptr);
    r.context->IASetVertexBuffers(0, 1, &noVB, &zero, &zero);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    r.context->VSSetShader(r.blitVS, nullptr, 0);
    r.context->PSSetShader(r.blitPS, nullptr, 0);
    r.context->PSSetShaderResources(0, 1, &r.offscreenSRV);
    r.context->PSSetSamplers(0, 1, &r.pointSampler);
    r.context->RSSetState(r.solidRS);
    const float blendFactor[4] = { 0, 0, 0, 0 };
    r.context->OMSetBlendState(r.opaqueBlendState, blendFactor, 0xFFFFFFFF);

    r.context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    r.context->PSSetShaderResources(0, 1, &nullSRV);
}

void RendererPresent(Renderer& r)
{
    r.swapChain->Present(1, 0);
}

void RendererRender(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    RendererRenderScene(r, scene, camera, sceneObj, assets);
    RendererBeginBackbufferOverlay(r);
    RendererPresent(r);
}

void RendererShutdown(Renderer& r)
{
    DestroyGPUTexture(r.whiteTexture);
    DestroyRendererAssetCaches(r);
    r.assetGeneration = 0;

    SafeRelease(r.pointSampler);
    SafeRelease(r.depthReadState);
    SafeRelease(r.depthWriteState);
    SafeRelease(r.alphaBlendState);
    SafeRelease(r.opaqueBlendState);
    SafeRelease(r.wireNoCullRS);
    SafeRelease(r.wireRS);
    SafeRelease(r.solidNoCullRS);
    SafeRelease(r.solidRS);

    SafeRelease(r.sceneCB);
    SafeRelease(r.skyCB);
    SafeRelease(r.modelCB);
    SafeRelease(r.materialCB);

    SafeRelease(r.blitPS);
    SafeRelease(r.blitVS);

    SafeRelease(r.skyPS);
    SafeRelease(r.skyVS);

    SafeRelease(r.meshLayout);
    SafeRelease(r.meshPS);
    SafeRelease(r.meshVS);

    DestroyRenderTargets(r);

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

        void RenderScene(const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets) override
        {
            kes::d3d11::RendererRenderScene(m_state, scene, camera, sceneObj, assets);
        }

        void BeginBackbufferOverlay() override { kes::d3d11::RendererBeginBackbufferOverlay(m_state); }
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

#pragma once
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
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
namespace tracy { class VkCtx; }
#endif

// Vulkan renderer backend. Internal to the engine: game code talks to the
// renderer through IRenderer (render_backend.h).
//
// Frame: shadow depth passes (sun cascades + local lights) -> HDR scene pass
// (RGBA16F, MSAA with resolve) -> tonemap into the swapchain image -> UI.
//
// Descriptor sets of mesh pipelines:
//   set 0  SceneCB, ModelCB (dynamic), MaterialCB (dynamic)
//   set 1  material textures (5 combined image samplers), one set per material
//   set 2  environment cube, cascade shadows, local shadows + samplers (one set)

class Scene;
class AssetSystem;
struct SceneObject;

namespace kes::vk
{

// GPU-side mesh: one device-local vertex buffer + optional index buffer.
struct GPUMesh
{
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
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

static constexpr uint32_t kMeshPipelineVariantCount = 16;

struct GPUShaderProgram
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule pixelShader = VK_NULL_HANDLE;
    VkPipeline pipelines[kMeshPipelineVariantCount] = {};
};

struct GPUTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // UI textures only (set 0 of the UI pipeline)
};

// Per-material texture set (set 1) and the Has*Texture flags it implies.
struct GPUMaterialSet
{
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t textureFlags = 0;
};

// Depth image array: one framebuffer per layer for rendering, one array view for sampling.
struct GPUShadowArray
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView arrayView = VK_NULL_HANDLE;
    std::vector<VkImageView> layerViews;
    std::vector<VkFramebuffer> framebuffers;
    int resolution = 0;
    int layers = 0;
};

struct GPUModelRenderCache
{
    std::vector<float> meshBindWorldMatrices;
};

struct GPUDynamicRingBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize slotStride = 0;   // per-draw slot size, aligned to minUniformBufferOffsetAlignment
    VkDeviceSize slotDataSize = 0; // logical (unaligned) struct size
    uint32_t slotCapacity = 0;     // number of slots currently allocated
    uint32_t cursor = 0;           // next free slot this frame
};

// Everything the CPU writes or records for one frame. kMaxFramesInFlight
// copies let the CPU record frame N+1 while the GPU still executes frame N
// (D3D11 drivers queue frames the same way behind the immediate context).
static constexpr uint32_t kMaxFramesInFlight = 2;

// Host-visible buffer rewritten every frame (UI vertices / indices). Grown
// when a frame needs more room; safe because the frame's fence was waited.
struct GPUStreamBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

// UI texture waiting for the frames that may still sample it to finish.
struct RetiredUiTexture
{
    GPUTexture texture;
    uint64_t lastUsableFrame = 0;   // frameNumber when it was released
};

struct FrameResources
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkFence inFlightFence = VK_NULL_HANDLE;

    VkBuffer sceneCB = VK_NULL_HANDLE;
    VkDeviceMemory sceneCBMemory = VK_NULL_HANDLE;
    void* sceneCBMapped = nullptr;

    VkBuffer skyCB = VK_NULL_HANDLE;
    VkDeviceMemory skyCBMemory = VK_NULL_HANDLE;
    void* skyCBMapped = nullptr;

    GPUDynamicRingBuffer modelCBRing;
    GPUDynamicRingBuffer materialCBRing;

    VkDescriptorSet uboDescriptorSet = VK_NULL_HANDLE; // points at this frame's sceneCB + rings
    VkDescriptorSet skyDescriptorSet = VK_NULL_HANDLE; // points at this frame's skyCB

    GPUStreamBuffer uiVertices;
    GPUStreamBuffer uiIndices;
};

struct DrawItem
{
    const SceneObject* object = nullptr;
    const MaterialData* material = nullptr;
    float distanceSq = 0.0f;
};

struct Renderer
{
    // Core objects
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties physicalDeviceProperties = {};
    bool anisotropySupported = false;

    uint32_t graphicsQueueFamily = 0;
    uint32_t presentQueueFamily = 0;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    // Swap chain / presentation
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent = {};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> backbufferFramebuffers;

    // Render passes
    VkRenderPass sceneRenderPass = VK_NULL_HANDLE;    // HDR color (+ resolve) + depth, msaaSamples
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;   // depth only
    VkRenderPass backbufferRenderPass = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;   // of sceneRenderPass

    // HDR scene targets (internal render resolution)
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat shadowFormat = VK_FORMAT_UNDEFINED;
    bool shadowLinearFilter = true;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    VkImage sceneColorMSImage = VK_NULL_HANDLE;
    VkDeviceMemory sceneColorMSMemory = VK_NULL_HANDLE;
    VkImageView sceneColorMSView = VK_NULL_HANDLE;
    VkImage sceneColorImage = VK_NULL_HANDLE;   // resolved, sampled by the tonemap
    VkDeviceMemory sceneColorMemory = VK_NULL_HANDLE;
    VkImageView sceneColorView = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet tonemapDescriptorSet = VK_NULL_HANDLE;

    // Shadows and image-based ambient
    GPUShadowArray cascadeShadows;
    GPUShadowArray localShadows;
    GPUShadowArray dummyShadows;   // 1x1, bound when shadows are off
    GPUTexture environmentMap = {};
    render::EnvironmentMaps environment;
    render::FrameSetup frame;
    VkDescriptorSet globalDescriptorSet = VK_NULL_HANDLE;   // set 2
    bool globalSetDirty = true;
    float exposure = 1.0f;
    GraphicsOptions options = {};

    // Descriptor / pipeline layouts
    VkDescriptorSetLayout uboSetLayout = VK_NULL_HANDLE;       // set 0: SceneCB, ModelCB(dyn), MaterialCB(dyn)
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;  // set 1: 5 combined image samplers
    VkDescriptorSetLayout globalSetLayout = VK_NULL_HANDLE;    // set 2: env, shadows, samplers
    VkDescriptorSetLayout texSetLayout = VK_NULL_HANDLE;       // tonemap set 0: combined image sampler (linear)
    VkDescriptorSetLayout skySetLayout = VK_NULL_HANDLE;       // sky set 0: SkyCB
    VkPipelineLayout meshPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout skyPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout tonemapPipelineLayout = VK_NULL_HANDLE;   // + push constant: exposure
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    GPUShaderProgram meshDefaultProgram;
    GPUShaderProgram shadowProgram;       // pipelines[0] = the shadow pipeline

    VkShaderModule skyVS = VK_NULL_HANDLE;
    VkShaderModule skyPS = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;

    VkShaderModule tonemapVS = VK_NULL_HANDLE;
    VkShaderModule tonemapPS = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline = VK_NULL_HANDLE;

    // UI pass (ui.slang): premultiplied alpha, scissor per draw, drawn inside
    // the backbuffer pass after the tonemap.
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout uiTexSetLayout = VK_NULL_HANDLE;   // set 0: combined image sampler (linear)
    VkPipelineLayout uiPipelineLayout = VK_NULL_HANDLE;      // + push constant: float4 scaleOffset
    VkShaderModule uiVS = VK_NULL_HANDLE;
    VkShaderModule uiPS = VK_NULL_HANDLE;
    VkPipeline uiPipeline = VK_NULL_HANDLE;
    VkDescriptorSet uiWhiteDescriptorSet = VK_NULL_HANDLE;   // whiteTexture through the UI layout
    std::unordered_map<UiTextureHandle, GPUTexture> uiTextures;
    std::vector<RetiredUiTexture> retiredUiTextures;
    UiTextureHandle nextUiTextureHandle = 1;
    bool backbufferPassOpen = false;   // BeginBackbufferOverlay leaves it open for the UI

    // Samplers / default texture
    VkSampler materialSampler = VK_NULL_HANDLE;   // anisotropic, repeat
    VkSampler shadowSampler = VK_NULL_HANDLE;     // depth comparison
    GPUTexture whiteTexture = {};

    // Command submission (kMaxFramesInFlight frames in flight)
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandPool transferCommandPool = VK_NULL_HANDLE;
    FrameResources frames[kMaxFramesInFlight];
    uint32_t frameIndex = 0;   // frames[frameIndex] is the frame being recorded
    uint64_t frameNumber = 0;  // frames submitted so far (deferred UI texture deletion)

    // Aliases of frames[frameIndex]'s handles, refreshed when a frame begins,
    // so recording code doesn't have to look up the current frame everywhere.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkDescriptorSet uboDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet skyDescriptorSet = VK_NULL_HANDLE;

    std::vector<VkSemaphore> renderFinishedSemaphores;   // one per swapchain image

    bool frameActive = false;       // between "start of frame" and RendererPresent
    std::string fatalError;         // IRenderer::GetFatalError

#if KES_PROFILE_GPU
    tracy::VkCtx* gpuProfiler = nullptr;   // Tracy GPU timestamp context
#endif
    bool haveSwapchainImage = false;
    bool swapchainNeedsRecreate = false;
    uint32_t currentSwapchainImageIndex = 0;

    // Asset caches
    std::unordered_map<MeshHandle, GPUMesh> gpuMeshes;
    std::unordered_map<GPUModelMeshKey, GPUMesh, GPUModelMeshKeyHasher> gpuModelMeshes;
    std::unordered_map<ShaderHandle, GPUShaderProgram> gpuShaders;
    std::unordered_map<std::string, GPUTexture> gpuTextures;   // key: usage + resolved path
    std::unordered_map<std::string, GPUMaterialSet> gpuMaterialSets;   // key: the five texture paths
    std::unordered_map<ModelHandle, GPUModelRenderCache> gpuModelRenderCaches;
    uint64_t assetGeneration = 0;

    int windowWidth = 0;
    int windowHeight = 0;
    int renderWidth = 0;
    int renderHeight = 0;
    bool preservePresentationAspect = true;
    bool preferIntegerPresentationScale = true;
    bool vsync = true;

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

} // namespace kes::vk

#pragma once
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
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
#include "profiler.h"

#if KES_PROFILE_GPU
namespace tracy { class VkCtx; }
#endif

// Vulkan renderer backend. Internal to the engine: game code talks to the
// renderer through IRenderer (render_backend.h).

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
    bool usesFogVarying = false; // true for the fallback (fog-capable) default shaders
};

struct GPUTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // set=1 binding=0 (combined image sampler)
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
    VkRenderPass offscreenRenderPass = VK_NULL_HANDLE;
    VkRenderPass backbufferRenderPass = VK_NULL_HANDLE;

    // Depth buffer (offscreen)
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    // Offscreen color target (rendered scene, blitted to backbuffer)
    VkImage offscreenColorImage = VK_NULL_HANDLE;
    VkDeviceMemory offscreenColorMemory = VK_NULL_HANDLE;
    VkImageView offscreenColorView = VK_NULL_HANDLE;
    VkFramebuffer offscreenFramebuffer = VK_NULL_HANDLE;
    VkDescriptorSet offscreenColorDescriptorSet = VK_NULL_HANDLE;

    // Descriptor / pipeline layouts
    VkDescriptorSetLayout uboSetLayout = VK_NULL_HANDLE;   // set 0 for mesh pipelines: SceneCB, ModelCB(dyn), MaterialCB(dyn)
    VkDescriptorSetLayout texSetLayout = VK_NULL_HANDLE;   // set 1 for mesh pipelines / set 0 for blit: combined image sampler
    VkDescriptorSetLayout skySetLayout = VK_NULL_HANDLE;   // set 0 for sky pipeline: SkyCB
    VkPipelineLayout meshPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout skyPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout blitPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Default (always-resident) mesh pipeline shaders
    GPUShaderProgram meshDefaultProgram; // r.meshVS / r.meshPS equivalent

    VkShaderModule skyVS = VK_NULL_HANDLE;
    VkShaderModule skyPS = VK_NULL_HANDLE;
    VkPipeline skyPipeline = VK_NULL_HANDLE;

    VkShaderModule blitVS = VK_NULL_HANDLE;
    VkShaderModule blitPS = VK_NULL_HANDLE;
    VkPipeline blitPipeline = VK_NULL_HANDLE;

    // Sampler / default texture
    VkSampler pointSampler = VK_NULL_HANDLE;
    GPUTexture whiteTexture = {};

    // Command submission (kMaxFramesInFlight frames in flight)
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandPool transferCommandPool = VK_NULL_HANDLE;
    FrameResources frames[kMaxFramesInFlight];
    uint32_t frameIndex = 0;   // frames[frameIndex] is the frame being recorded

    // Aliases of frames[frameIndex]'s handles, refreshed when a frame begins,
    // so recording code doesn't have to look up the current frame everywhere.
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkDescriptorSet uboDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet skyDescriptorSet = VK_NULL_HANDLE;

    std::vector<VkSemaphore> renderFinishedSemaphores;   // one per swapchain image

    bool frameActive = false;       // between "start of frame" and RendererPresent

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

} // namespace kes::vk

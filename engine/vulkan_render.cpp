#include "vulkan_render.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstddef>
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

#if KES_PROFILE_GPU
#include <tracy/TracyVulkan.hpp>
// GPU timestamp zone recorded into this frame's command buffer.
#define KES_VK_GPU_ZONE(r, name) TracyVkZone((r).gpuProfiler, (r).commandBuffer, name)
#else
#define KES_VK_GPU_ZONE(r, name) ((void)0)
#endif

namespace kes::vk
{


// Math helpers, kept identical to d3d11_render.cpp so both backends compute
// the same world/view/projection matrices.


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



static bool CreateWhiteTexture(Renderer& r);
static bool CreateTextureFromFile(Renderer& r, const char* path, GPUTexture& outTexture);
static VkDescriptorSet GetMaterialTextureDescriptor(Renderer& r, const MaterialData* material);
static void DestroyRendererAssetCaches(Renderer& r);
static const GPUModelRenderCache* GetOrBuildModelRenderCache(Renderer& r, ModelHandle handle, const AssetSystem& assets);
static void DestroyGPUShaderProgram(Renderer& r, GPUShaderProgram& program);
static void DestroyGPUTexture(Renderer& r, GPUTexture& texture);


// Shaders are authored once in Slang (engine/shaders/*.slang) and compiled to
// SPIR-V at build time (cmake/KesShaders.cmake). Nothing is compiled at runtime.

// Small Vulkan utility helpers


#define VK_CHECK_RET(expr, msg, retval) \
    do { VkResult vkCheckResult_ = (expr); if (vkCheckResult_ != VK_SUCCESS) { \
        EngineLogf("%s failed: %d", msg, static_cast<int>(vkCheckResult_)); return retval; } } while (0)

static uint32_t FindMemoryType(Renderer& r, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties(r.physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    EngineLogf("FindMemoryType: no suitable memory type found (typeBits=0x%08X)", typeBits);
    return UINT32_MAX;
}

static VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
    if (alignment == 0)
        return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool CreateBuffer(Renderer& r, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& outBuffer, VkDeviceMemory& outMemory)
{
    VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK_RET(vkCreateBuffer(r.device, &bufferInfo, nullptr, &outBuffer), "vkCreateBuffer", false);

    VkMemoryRequirements memReq = {};
    vkGetBufferMemoryRequirements(r.device, outBuffer, &memReq);

    const uint32_t memoryType = FindMemoryType(r, memReq.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX)
    {
        vkDestroyBuffer(r.device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryType;

    if (vkAllocateMemory(r.device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
    {
        EngineLogf("vkAllocateMemory failed for buffer");
        vkDestroyBuffer(r.device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(r.device, outBuffer, outMemory, 0);
    return true;
}

static void DestroyBuffer(Renderer& r, VkBuffer& buffer, VkDeviceMemory& memory)
{
    if (buffer) vkDestroyBuffer(r.device, buffer, nullptr);
    if (memory) vkFreeMemory(r.device, memory, nullptr);
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

static VkCommandBuffer BeginSingleTimeCommands(Renderer& r)
{
    VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = r.transferCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(r.device, &allocInfo, &cmd) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

static void EndSingleTimeCommands(Renderer& r, VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(r.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(r.graphicsQueue); // asset loading is not perf critical here; mirrors D3D11's synchronous immediate-context uploads

    vkFreeCommandBuffers(r.device, r.transferCommandPool, 1, &cmd);
}

static bool CreateDeviceLocalBufferWithData(Renderer& r, const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VkDeviceMemory& outMemory)
{
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateBuffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory))
        return false;

    void* mapped = nullptr;
    vkMapMemory(r.device, stagingMemory, 0, size, 0, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(r.device, stagingMemory);

    if (!CreateBuffer(r, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer, outMemory))
    {
        DestroyBuffer(r, stagingBuffer, stagingMemory);
        return false;
    }

    VkCommandBuffer cmd = BeginSingleTimeCommands(r);
    if (!cmd)
    {
        DestroyBuffer(r, stagingBuffer, stagingMemory);
        DestroyBuffer(r, outBuffer, outMemory);
        return false;
    }

    VkBufferCopy copyRegion = {};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, stagingBuffer, outBuffer, 1, &copyRegion);
    EndSingleTimeCommands(r, cmd);

    DestroyBuffer(r, stagingBuffer, stagingMemory);
    return true;
}

static bool CreateImage2D(Renderer& r, uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
    VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& outImage, VkDeviceMemory& outMemory)
{
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK_RET(vkCreateImage(r.device, &imageInfo, nullptr, &outImage), "vkCreateImage", false);

    VkMemoryRequirements memReq = {};
    vkGetImageMemoryRequirements(r.device, outImage, &memReq);

    const uint32_t memoryType = FindMemoryType(r, memReq.memoryTypeBits, properties);
    if (memoryType == UINT32_MAX)
    {
        vkDestroyImage(r.device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryType;

    if (vkAllocateMemory(r.device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
    {
        vkDestroyImage(r.device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(r.device, outImage, outMemory, 0);
    return true;
}

static VkImageView CreateImageView2D(Renderer& r, VkImage image, VkFormat format, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(r.device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        EngineLogf("vkCreateImageView failed");
        return VK_NULL_HANDLE;
    }
    return view;
}

static void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        // Generic fallback: coarse but correct full barrier.
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Creates a shader module from a precompiled SPIR-V binary produced by slangc.
static VkShaderModule CreateShaderModuleFromBinary(Renderer& r, const std::string& sourcePath, ShaderStage stage)
{
    std::vector<uint8_t> bytes;
    std::string error;
    if (!LoadCompiledShader(sourcePath, stage, RendererBackend::Vulkan, bytes, &error))
    {
        EngineLogf("%s", error.c_str());
        return VK_NULL_HANDLE;
    }
    if (bytes.size() % sizeof(uint32_t) != 0)
    {
        EngineLogf("Invalid SPIR-V size for %s", BuildCompiledShaderPath(sourcePath, stage, RendererBackend::Vulkan).c_str());
        return VK_NULL_HANDLE;
    }

    // Copy into a uint32_t buffer to guarantee the alignment Vulkan requires.
    std::vector<uint32_t> spirv(bytes.size() / sizeof(uint32_t));
    std::memcpy(spirv.data(), bytes.data(), bytes.size());

    VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = bytes.size();
    moduleInfo.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(r.device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
    {
        EngineLogf("vkCreateShaderModule failed for %s", BuildCompiledShaderPath(sourcePath, stage, RendererBackend::Vulkan).c_str());
        return VK_NULL_HANDLE;
    }
    return module;
}


// Instance / device / swap chain creation


static VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
    (void)type;
    (void)userData;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        EngineLogf("[vulkan] %s", callbackData->pMessage);
    return VK_FALSE;
}

static bool CreateInstance(Renderer& r, SDL_Window* window)
{
    Uint32 sdlExtensionCount = 0;
    char const* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (!sdlExtensions && sdlExtensionCount > 0)
    {
        EngineLogf("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);

#ifdef _DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "Application";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "KES";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();

#ifdef _DEBUG
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (const VkLayerProperties& layer : layers)
    {
        if (std::strcmp(layer.layerName, validationLayer) == 0) { haveValidation = true; break; }
    }
    if (haveValidation)
    {
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &validationLayer;
    }
#endif

    VK_CHECK_RET(vkCreateInstance(&instanceInfo, nullptr, &r.instance), "vkCreateInstance", false);

#ifdef _DEBUG
    if (haveValidation)
    {
        auto createDebugUtils = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(r.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createDebugUtils)
        {
            VkDebugUtilsMessengerCreateInfoEXT dbgInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbgInfo.pfnUserCallback = DebugMessengerCallback;
            createDebugUtils(r.instance, &dbgInfo, nullptr, &r.debugMessenger);
        }
    }
#endif

    if (!SDL_Vulkan_CreateSurface(window, r.instance, nullptr, &r.surface))
    {
        EngineLogf("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

struct PhysicalDeviceCandidate
{
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t presentFamily = UINT32_MAX;
    bool discrete = false;
};

static bool QueryQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface, uint32_t& outGraphics, uint32_t& outPresent)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    outGraphics = UINT32_MAX;
    outPresent = UINT32_MAX;

    for (uint32_t i = 0; i < count; ++i)
    {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && outGraphics == UINT32_MAX)
            outGraphics = i;

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
        if (presentSupport && outPresent == UINT32_MAX)
            outPresent = i;

        if (outGraphics != UINT32_MAX && outPresent != UINT32_MAX && outGraphics == outPresent)
            break;
    }

    // Prefer a combined graphics+present queue but fall back to separate ones.
    if (outPresent == UINT32_MAX)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport) { outPresent = i; break; }
        }
    }

    return outGraphics != UINT32_MAX && outPresent != UINT32_MAX;
}

static bool DeviceSupportsSwapchain(VkPhysicalDevice device)
{
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, exts.data());

    for (const VkExtensionProperties& ext : exts)
    {
        if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
            return true;
    }
    return false;
}

static bool SelectPhysicalDevice(Renderer& r)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(r.instance, &count, nullptr);
    if (count == 0)
    {
        EngineLogf("No Vulkan-capable physical devices found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(r.instance, &count, devices.data());

    PhysicalDeviceCandidate best;

    for (VkPhysicalDevice device : devices)
    {
        if (!DeviceSupportsSwapchain(device))
            continue;

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, r.surface, &formatCount, nullptr);
        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, r.surface, &presentModeCount, nullptr);
        if (formatCount == 0 || presentModeCount == 0)
            continue;

        uint32_t graphicsFamily = UINT32_MAX, presentFamily = UINT32_MAX;
        if (!QueryQueueFamilies(device, r.surface, graphicsFamily, presentFamily))
            continue;

        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(device, &props);

        const bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (best.device == VK_NULL_HANDLE || (discrete && !best.discrete))
        {
            best.device = device;
            best.graphicsFamily = graphicsFamily;
            best.presentFamily = presentFamily;
            best.discrete = discrete;
        }
    }

    if (best.device == VK_NULL_HANDLE)
    {
        EngineLogf("No suitable Vulkan physical device found");
        return false;
    }

    r.physicalDevice = best.device;
    r.graphicsQueueFamily = best.graphicsFamily;
    r.presentQueueFamily = best.presentFamily;
    vkGetPhysicalDeviceProperties(r.physicalDevice, &r.physicalDeviceProperties);
    return true;
}

static bool CreateLogicalDevice(Renderer& r)
{
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const float priority = 1.0f;

    VkDeviceQueueCreateInfo graphicsQueueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    graphicsQueueInfo.queueFamilyIndex = r.graphicsQueueFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &priority;
    queueInfos.push_back(graphicsQueueInfo);

    if (r.presentQueueFamily != r.graphicsQueueFamily)
    {
        VkDeviceQueueCreateInfo presentQueueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        presentQueueInfo.queueFamilyIndex = r.presentQueueFamily;
        presentQueueInfo.queueCount = 1;
        presentQueueInfo.pQueuePriorities = &priority;
        queueInfos.push_back(presentQueueInfo);
    }

    VkPhysicalDeviceFeatures features = {};
    features.fillModeNonSolid = VK_TRUE; // required for the wireframe rasterizer variants (F1 debug toggle)
    features.wideLines = VK_TRUE;
    features.samplerAnisotropy = VK_FALSE;

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo deviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.pEnabledFeatures = &features;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    VK_CHECK_RET(vkCreateDevice(r.physicalDevice, &deviceInfo, nullptr, &r.device), "vkCreateDevice", false);

    vkGetDeviceQueue(r.device, r.graphicsQueueFamily, 0, &r.graphicsQueue);
    vkGetDeviceQueue(r.device, r.presentQueueFamily, 0, &r.presentQueue);
    return true;
}

static VkSurfaceFormatKHR ChooseSurfaceFormat(Renderer& r)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physicalDevice, r.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.physicalDevice, r.surface, &count, formats.data());

    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }
    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }
    return formats[0];
}

static VkPresentModeKHR ChoosePresentMode(Renderer& r)
{
    // FIFO is always available and is vsync. Without vsync prefer IMMEDIATE
    // (lowest latency, may tear), then MAILBOX (uncapped, no tearing).
    if (r.vsync)
        return VK_PRESENT_MODE_FIFO_KHR;

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(r.physicalDevice, r.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(r.physicalDevice, r.surface, &count, modes.data());

    for (VkPresentModeKHR preferred : { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR })
    {
        if (std::find(modes.begin(), modes.end(), preferred) != modes.end())
            return preferred;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D ChooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& caps,
    int windowWidth,
    int windowHeight)
{
    if (caps.currentExtent.width != UINT32_MAX)
        return caps.currentExtent;

    VkExtent2D extent = {
        static_cast<uint32_t>(windowWidth),
        static_cast<uint32_t>(windowHeight)
    };

    if (extent.width < caps.minImageExtent.width)
        extent.width = caps.minImageExtent.width;
    else if (extent.width > caps.maxImageExtent.width)
        extent.width = caps.maxImageExtent.width;

    if (extent.height < caps.minImageExtent.height)
        extent.height = caps.minImageExtent.height;
    else if (extent.height > caps.maxImageExtent.height)
        extent.height = caps.maxImageExtent.height;

    return extent;
}

static void DestroySwapchainObjects(Renderer& r)
{
    for (VkSemaphore semaphore : r.renderFinishedSemaphores)
        vkDestroySemaphore(r.device, semaphore, nullptr);
    r.renderFinishedSemaphores.clear();

    for (VkFramebuffer fb : r.backbufferFramebuffers)
        vkDestroyFramebuffer(r.device, fb, nullptr);
    r.backbufferFramebuffers.clear();

    for (VkImageView view : r.swapchainImageViews)
        vkDestroyImageView(r.device, view, nullptr);
    r.swapchainImageViews.clear();
    r.swapchainImages.clear();

    if (r.swapchain)
    {
        vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
        r.swapchain = VK_NULL_HANDLE;
    }
}

static bool CreateSwapchain(Renderer& r)
{
    VkSurfaceCapabilitiesKHR caps = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.physicalDevice, r.surface, &caps);

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(r);
    const VkPresentModeKHR presentMode = ChoosePresentMode(r);
    const VkExtent2D extent = ChooseSwapExtent(caps, r.windowWidth, r.windowHeight);

    if (extent.width == 0 || extent.height == 0)
        return false; // window minimized; caller should retry later

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR swapInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapInfo.surface = r.surface;
    swapInfo.minImageCount = imageCount;
    swapInfo.imageFormat = surfaceFormat.format;
    swapInfo.imageColorSpace = surfaceFormat.colorSpace;
    swapInfo.imageExtent = extent;
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const uint32_t queueFamilyIndices[] = { r.graphicsQueueFamily, r.presentQueueFamily };
    if (r.graphicsQueueFamily != r.presentQueueFamily)
    {
        swapInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapInfo.queueFamilyIndexCount = 2;
        swapInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = presentMode;
    swapInfo.clipped = VK_TRUE;
    swapInfo.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK_RET(vkCreateSwapchainKHR(r.device, &swapInfo, nullptr, &r.swapchain), "vkCreateSwapchainKHR", false);

    r.swapchainFormat = surfaceFormat.format;
    r.swapchainExtent = extent;

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(r.device, r.swapchain, &actualImageCount, nullptr);
    r.swapchainImages.resize(actualImageCount);
    vkGetSwapchainImagesKHR(r.device, r.swapchain, &actualImageCount, r.swapchainImages.data());

    r.swapchainImageViews.resize(actualImageCount);
    for (uint32_t i = 0; i < actualImageCount; ++i)
    {
        r.swapchainImageViews[i] = CreateImageView2D(r, r.swapchainImages[i], r.swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!r.swapchainImageViews[i])
            return false;
    }

    // One "render finished" semaphore per swapchain image: the presentation
    // engine may still hold the semaphore of an image until that image is
    // acquired again, so a single shared semaphore is not safe to reuse.
    r.renderFinishedSemaphores.resize(actualImageCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < actualImageCount; ++i)
        VK_CHECK_RET(vkCreateSemaphore(r.device, &semaphoreInfo, nullptr, &r.renderFinishedSemaphores[i]), "vkCreateSemaphore (renderFinished)", false);

    return true;
}


// Render passes


static bool ChooseDepthFormat(Renderer& r)
{
    const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT };
    for (VkFormat format : candidates)
    {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(r.physicalDevice, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            r.depthFormat = format;
            return true;
        }
    }
    EngineLogf("No suitable depth/stencil format found");
    return false;
}

static bool CreateOffscreenRenderPass(Renderer& r)
{
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment = {};
    depthAttachment.format = r.depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    // Previous frame's blit read of the color target (WAR) and its depth
    // writes (WAW): with several frames in flight, frames overlap on the GPU.
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkAttachmentDescription attachments[2] = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo passInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 2;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;

    VK_CHECK_RET(vkCreateRenderPass(r.device, &passInfo, nullptr, &r.offscreenRenderPass), "vkCreateRenderPass (offscreen)", false);
    return true;
}

static bool CreateBackbufferRenderPass(Renderer& r)
{
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = r.swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo passInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &colorAttachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;

    VK_CHECK_RET(vkCreateRenderPass(r.device, &passInfo, nullptr, &r.backbufferRenderPass), "vkCreateRenderPass (backbuffer)", false);
    return true;
}


// Descriptor / pipeline layouts (created once; independent of window size)


static bool CreateDescriptorSetLayouts(Renderer& r)
{
    // set 0 for mesh pipelines: SceneCB (fixed), ModelCB (dynamic), MaterialCB (dynamic)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo uboLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    uboLayoutInfo.bindingCount = 3;
    uboLayoutInfo.pBindings = bindings;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &uboLayoutInfo, nullptr, &r.uboSetLayout), "vkCreateDescriptorSetLayout (ubo)", false);

    // set 1 for mesh pipelines / set 0 for blit: a single combined image sampler
    // with an immutable point sampler baked in (matches the single shared
    // r.pointSampler used for every texture in the D3D11 renderer).
    VkDescriptorSetLayoutBinding texBinding = {};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    texBinding.pImmutableSamplers = &r.pointSampler;

    VkDescriptorSetLayoutCreateInfo texLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    texLayoutInfo.bindingCount = 1;
    texLayoutInfo.pBindings = &texBinding;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &texLayoutInfo, nullptr, &r.texSetLayout), "vkCreateDescriptorSetLayout (tex)", false);

    // set 0 for the UI pipeline: same shape, linear immutable sampler.
    VkDescriptorSetLayoutBinding uiTexBinding = texBinding;
    uiTexBinding.pImmutableSamplers = &r.linearSampler;
    VkDescriptorSetLayoutCreateInfo uiTexLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    uiTexLayoutInfo.bindingCount = 1;
    uiTexLayoutInfo.pBindings = &uiTexBinding;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &uiTexLayoutInfo, nullptr, &r.uiTexSetLayout), "vkCreateDescriptorSetLayout (ui tex)", false);

    // set 0 for the sky pipeline: SkyCB only.
    VkDescriptorSetLayoutBinding skyBinding = {};
    skyBinding.binding = 0;
    skyBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    skyBinding.descriptorCount = 1;
    skyBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo skyLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    skyLayoutInfo.bindingCount = 1;
    skyLayoutInfo.pBindings = &skyBinding;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &skyLayoutInfo, nullptr, &r.skySetLayout), "vkCreateDescriptorSetLayout (sky)", false);

    return true;
}

static bool CreatePipelineLayouts(Renderer& r)
{
    VkDescriptorSetLayout meshSets[2] = { r.uboSetLayout, r.texSetLayout };
    VkPipelineLayoutCreateInfo meshLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    meshLayoutInfo.setLayoutCount = 2;
    meshLayoutInfo.pSetLayouts = meshSets;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &meshLayoutInfo, nullptr, &r.meshPipelineLayout), "vkCreatePipelineLayout (mesh)", false);

    VkPipelineLayoutCreateInfo skyLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    skyLayoutInfo.setLayoutCount = 1;
    skyLayoutInfo.pSetLayouts = &r.skySetLayout;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &skyLayoutInfo, nullptr, &r.skyPipelineLayout), "vkCreatePipelineLayout (sky)", false);

    VkPipelineLayoutCreateInfo blitLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    blitLayoutInfo.setLayoutCount = 1;
    blitLayoutInfo.pSetLayouts = &r.texSetLayout;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &blitLayoutInfo, nullptr, &r.blitPipelineLayout), "vkCreatePipelineLayout (blit)", false);

    VkPushConstantRange uiPushRange = {};
    uiPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uiPushRange.offset = 0;
    uiPushRange.size = sizeof(float) * 4;   // ui.slang UiConstants
    VkPipelineLayoutCreateInfo uiLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    uiLayoutInfo.setLayoutCount = 1;
    uiLayoutInfo.pSetLayouts = &r.uiTexSetLayout;
    uiLayoutInfo.pushConstantRangeCount = 1;
    uiLayoutInfo.pPushConstantRanges = &uiPushRange;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &uiLayoutInfo, nullptr, &r.uiPipelineLayout), "vkCreatePipelineLayout (ui)", false);

    return true;
}

static bool CreateDescriptorPool(Renderer& r)
{
    // Sized generously: texSetLayout sets are allocated per unique texture
    // (materials, white texture, offscreen color), uboSetLayout/skySetLayout
    // once per frame in flight.
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = 8;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[1].descriptorCount = 8;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 4096;

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4096 + 8;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;

    VK_CHECK_RET(vkCreateDescriptorPool(r.device, &poolInfo, nullptr, &r.descriptorPool), "vkCreateDescriptorPool", false);
    return true;
}

static VkDescriptorSet AllocateTextureDescriptorSet(Renderer& r, VkImageView view, VkDescriptorSetLayout layout = VK_NULL_HANDLE)
{
    if (!layout)
        layout = r.texSetLayout;

    VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = r.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(r.device, &allocInfo, &set) != VK_SUCCESS)
    {
        EngineLogf("vkAllocateDescriptorSets (texture) failed");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = view;
    imageInfo.sampler = layout == r.uiTexSetLayout ? r.linearSampler : r.pointSampler; // ignored (immutable sampler), set for clarity/validation

    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(r.device, 1, &write, 0, nullptr);
    return set;
}

static bool CreateUboDescriptorSets(Renderer& r)
{
    for (FrameResources& frame : r.frames)
    {
        VkDescriptorSetAllocateInfo uboAllocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        uboAllocInfo.descriptorPool = r.descriptorPool;
        uboAllocInfo.descriptorSetCount = 1;
        uboAllocInfo.pSetLayouts = &r.uboSetLayout;
        VK_CHECK_RET(vkAllocateDescriptorSets(r.device, &uboAllocInfo, &frame.uboDescriptorSet), "vkAllocateDescriptorSets (ubo)", false);

        VkDescriptorSetAllocateInfo skyAllocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        skyAllocInfo.descriptorPool = r.descriptorPool;
        skyAllocInfo.descriptorSetCount = 1;
        skyAllocInfo.pSetLayouts = &r.skySetLayout;
        VK_CHECK_RET(vkAllocateDescriptorSets(r.device, &skyAllocInfo, &frame.skyDescriptorSet), "vkAllocateDescriptorSets (sky)", false);
    }

    return true;
}

static void UpdateSceneAndSkyDescriptorBindings(Renderer& r, const FrameResources& frame)
{
    VkDescriptorBufferInfo sceneInfo = { frame.sceneCB, 0, sizeof(SceneCBData) };
    VkDescriptorBufferInfo skyInfo = { frame.skyCB, 0, sizeof(SkyCBData) };

    VkWriteDescriptorSet writes[2] = {};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    writes[0].dstSet = frame.uboDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &sceneInfo;

    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    writes[1].dstSet = frame.skyDescriptorSet;
    writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &skyInfo;

    vkUpdateDescriptorSets(r.device, 2, writes, 0, nullptr);
}

// Updates just the ModelCB or MaterialCB dynamic-buffer binding on the
// frame's uboDescriptorSet; called once at creation and again whenever
// a ring buffer grows and gets a new underlying VkBuffer.
static void UpdateDynamicRingDescriptorBinding(Renderer& r, VkDescriptorSet set, uint32_t binding, const GPUDynamicRingBuffer& ring)
{
    VkDescriptorBufferInfo info = { ring.buffer, 0, ring.slotDataSize };

    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    write.pBufferInfo = &info;

    vkUpdateDescriptorSets(r.device, 1, &write, 0, nullptr);
}

// Dynamic uniform ring buffers (ModelCB / MaterialCB per-draw slots)
static constexpr uint32_t kInitialDynamicSlotCapacity = 2048;

static bool CreateDynamicRingBuffer(Renderer& r, GPUDynamicRingBuffer& ring, VkDeviceSize slotDataSize, uint32_t slotCapacity)
{
    ring.slotDataSize = slotDataSize;
    const VkDeviceSize alignment = r.physicalDeviceProperties.limits.minUniformBufferOffsetAlignment;
    ring.slotStride = AlignUp(slotDataSize, alignment == 0 ? 1 : alignment);
    ring.slotCapacity = slotCapacity;
    ring.cursor = 0;

    if (!CreateBuffer(r, ring.slotStride * slotCapacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        ring.buffer, ring.memory))
        return false;

    VK_CHECK_RET(vkMapMemory(r.device, ring.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&ring.mapped)), "vkMapMemory (ring)", false);
    return true;
}

static void DestroyDynamicRingBuffer(Renderer& r, GPUDynamicRingBuffer& ring)
{
    if (ring.mapped) vkUnmapMemory(r.device, ring.memory);
    DestroyBuffer(r, ring.buffer, ring.memory);
    ring.mapped = nullptr;
    ring.slotCapacity = 0;
    ring.cursor = 0;
}

// Grows a ring buffer in place, preserving already-written slot contents
// (so draws already recorded into the in-flight command buffer, which
// reference the descriptor set by handle rather than by snapshot, keep
// seeing correct data once the descriptor's underlying buffer is repointed
// at the new, larger allocation).
static bool GrowDynamicRingBuffer(Renderer& r, GPUDynamicRingBuffer& ring, uint32_t requiredSlots, uint32_t descriptorBinding)
{
    uint32_t newCapacity = ring.slotCapacity == 0 ? kInitialDynamicSlotCapacity : ring.slotCapacity;
    while (newCapacity < requiredSlots)
        newCapacity *= 2;

    // Nothing may currently be executing on the GPU that references this
    // frame's command buffer yet (it has not been submitted), so it is safe
    // to reallocate synchronously here.
    vkDeviceWaitIdle(r.device);

    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    if (!CreateBuffer(r, ring.slotStride * newCapacity, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        newBuffer, newMemory))
        return false;

    uint8_t* newMapped = nullptr;
    if (vkMapMemory(r.device, newMemory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&newMapped)) != VK_SUCCESS)
    {
        DestroyBuffer(r, newBuffer, newMemory);
        return false;
    }

    if (ring.mapped && ring.cursor > 0)
        std::memcpy(newMapped, ring.mapped, static_cast<size_t>(ring.slotStride) * ring.cursor);

    DestroyDynamicRingBuffer(r, ring);
    ring.buffer = newBuffer;
    ring.memory = newMemory;
    ring.mapped = newMapped;
    ring.slotCapacity = newCapacity;

    UpdateDynamicRingDescriptorBinding(r, r.frames[r.frameIndex].uboDescriptorSet, descriptorBinding, ring);
    return true;
}

// Reserves the next slot, growing the buffer first if needed, and returns
// the byte offset to use both for the memcpy destination and the dynamic
// descriptor offset passed to vkCmdBindDescriptorSets.
static VkDeviceSize AcquireDynamicSlot(Renderer& r, GPUDynamicRingBuffer& ring, uint32_t descriptorBinding)
{
    if (ring.cursor + 1 > ring.slotCapacity)
    {
        if (!GrowDynamicRingBuffer(r, ring, ring.cursor + 1, descriptorBinding))
        {
            EngineLogf("GrowDynamicRingBuffer failed; reusing last slot (visual corruption may occur)");
            return static_cast<VkDeviceSize>(ring.slotStride) * (ring.slotCapacity > 0 ? ring.slotCapacity - 1 : 0);
        }
    }

    const VkDeviceSize offset = static_cast<VkDeviceSize>(ring.slotStride) * ring.cursor;
    ring.cursor++;
    return offset;
}

// Sampler / render targets
static bool CreateSampler(Renderer& r)
{
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

    VK_CHECK_RET(vkCreateSampler(r.device, &samplerInfo, nullptr, &r.pointSampler), "vkCreateSampler", false);

    // UI images and transformed text (ui.slang) use bilinear filtering.
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VK_CHECK_RET(vkCreateSampler(r.device, &samplerInfo, nullptr, &r.linearSampler), "vkCreateSampler (linear)", false);
    return true;
}

static void DestroyRenderTargets(Renderer& r)
{
    if (r.offscreenColorDescriptorSet)
    {
        vkFreeDescriptorSets(r.device, r.descriptorPool, 1, &r.offscreenColorDescriptorSet);
        r.offscreenColorDescriptorSet = VK_NULL_HANDLE;
    }

    if (r.offscreenFramebuffer) { vkDestroyFramebuffer(r.device, r.offscreenFramebuffer, nullptr); r.offscreenFramebuffer = VK_NULL_HANDLE; }
    if (r.offscreenColorView) { vkDestroyImageView(r.device, r.offscreenColorView, nullptr); r.offscreenColorView = VK_NULL_HANDLE; }
    if (r.offscreenColorImage) { vkDestroyImage(r.device, r.offscreenColorImage, nullptr); r.offscreenColorImage = VK_NULL_HANDLE; }
    if (r.offscreenColorMemory) { vkFreeMemory(r.device, r.offscreenColorMemory, nullptr); r.offscreenColorMemory = VK_NULL_HANDLE; }

    if (r.depthView) { vkDestroyImageView(r.device, r.depthView, nullptr); r.depthView = VK_NULL_HANDLE; }
    if (r.depthImage) { vkDestroyImage(r.device, r.depthImage, nullptr); r.depthImage = VK_NULL_HANDLE; }
    if (r.depthMemory) { vkFreeMemory(r.device, r.depthMemory, nullptr); r.depthMemory = VK_NULL_HANDLE; }

    DestroySwapchainObjects(r);
}

static bool CreateRenderTargets(Renderer& r)
{
    if (!CreateSwapchain(r))
        return false;

    if (!r.backbufferRenderPass)
    {
        if (!CreateBackbufferRenderPass(r))
            return false;
    }

    if (!r.depthFormat && !ChooseDepthFormat(r))
        return false;

    if (!r.offscreenRenderPass)
    {
        if (!CreateOffscreenRenderPass(r))
            return false;
    }

    // Depth buffer, sized to the internal render resolution (matches
    // r.renderWidth/renderHeight in the D3D11 version, not the window size).
    if (!CreateImage2D(r, static_cast<uint32_t>(r.renderWidth), static_cast<uint32_t>(r.renderHeight), r.depthFormat,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        r.depthImage, r.depthMemory))
        return false;
    r.depthView = CreateImageView2D(r, r.depthImage, r.depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (!r.depthView)
        return false;

    // Offscreen color target the scene renders into, later blitted (via the
    // fullscreen blit pipeline) onto the swap chain image.
    if (!CreateImage2D(r, static_cast<uint32_t>(r.renderWidth), static_cast<uint32_t>(r.renderHeight), VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        r.offscreenColorImage, r.offscreenColorMemory))
        return false;
    r.offscreenColorView = CreateImageView2D(r, r.offscreenColorImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!r.offscreenColorView)
        return false;

    VkImageView offscreenAttachments[2] = { r.offscreenColorView, r.depthView };
    VkFramebufferCreateInfo offscreenFbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    offscreenFbInfo.renderPass = r.offscreenRenderPass;
    offscreenFbInfo.attachmentCount = 2;
    offscreenFbInfo.pAttachments = offscreenAttachments;
    offscreenFbInfo.width = static_cast<uint32_t>(r.renderWidth);
    offscreenFbInfo.height = static_cast<uint32_t>(r.renderHeight);
    offscreenFbInfo.layers = 1;
    VK_CHECK_RET(vkCreateFramebuffer(r.device, &offscreenFbInfo, nullptr, &r.offscreenFramebuffer), "vkCreateFramebuffer (offscreen)", false);

    r.offscreenColorDescriptorSet = AllocateTextureDescriptorSet(r, r.offscreenColorView);
    if (!r.offscreenColorDescriptorSet)
        return false;

    // Backbuffer framebuffers, one per swap chain image.
    r.backbufferFramebuffers.resize(r.swapchainImageViews.size());
    for (size_t i = 0; i < r.swapchainImageViews.size(); ++i)
    {
        VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbInfo.renderPass = r.backbufferRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &r.swapchainImageViews[i];
        fbInfo.width = r.swapchainExtent.width;
        fbInfo.height = r.swapchainExtent.height;
        fbInfo.layers = 1;
        VK_CHECK_RET(vkCreateFramebuffer(r.device, &fbInfo, nullptr, &r.backbufferFramebuffers[i]), "vkCreateFramebuffer (backbuffer)", false);
    }

    return true;
}


// Mesh pipeline permutations

enum MeshPipelineVariantBit : uint32_t
{
    kVariantBit_Wireframe = 1u << 0,
    kVariantBit_DoubleSided = 1u << 1,
    kVariantBit_Transparent = 1u << 2,
    kVariantBit_DepthReadOnly = 1u << 3,
};

static uint32_t SelectMeshPipelineVariant(bool wireframe, bool doubleSided, bool transparent, bool depthReadOnly)
{
    uint32_t v = 0;
    if (wireframe) v |= kVariantBit_Wireframe;
    if (doubleSided) v |= kVariantBit_DoubleSided;
    if (transparent) v |= kVariantBit_Transparent;
    if (depthReadOnly) v |= kVariantBit_DepthReadOnly;
    return v;
}

static VkPipeline CreateMeshPipelineVariant(Renderer& r, const GPUShaderProgram& program, uint32_t variant)
{
    const bool wireframe = (variant & kVariantBit_Wireframe) != 0;
    const bool doubleSided = (variant & kVariantBit_DoubleSided) != 0;
    const bool transparent = (variant & kVariantBit_Transparent) != 0;
    const bool depthReadOnly = (variant & kVariantBit_DepthReadOnly) != 0;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = program.vertexShader;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = program.pixelShader;
    stages[1].pName = "main";

    // Matches RenderVertex exactly: pos(vec3)+normal(vec3)+uv(vec2)+color(vec4)+boneIndices(uint16x4)+boneWeights(vec4)
    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(RenderVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[6] = {};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderVertex, px) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderVertex, nx) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(RenderVertex, u) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RenderVertex, r) };
    attrs[4] = { 4, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(RenderVertex, boneIndices) };
    attrs[5] = { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RenderVertex, boneWeights) };

    VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 6;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE; // matches D3D11's default (CCW culled as back-facing) under the negative-viewport-height flip
    rasterizer.lineWidth = 1.0f;
    rasterizer.depthClampEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = depthReadOnly ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (transparent)
    {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    else
    {
        blendAttachment.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = r.meshPipelineLayout;
    pipelineInfo.renderPass = r.offscreenRenderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        EngineLogf("vkCreateGraphicsPipelines (mesh variant 0x%X) failed", variant);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// Lazily builds (and caches) the pipeline for the requested state
// combination - the Vulkan analogue of SelectRasterizerState() combined
// with the OMSetBlendState/OMSetDepthStencilState calls around it.
static VkPipeline GetOrCreateMeshPipelineVariant(Renderer& r, GPUShaderProgram& program, bool wireframe, bool doubleSided, bool transparent)
{
    const bool depthReadOnly = transparent;
    const uint32_t variant = SelectMeshPipelineVariant(wireframe, doubleSided, transparent, depthReadOnly);

    if (program.pipelines[variant] == VK_NULL_HANDLE)
    {
        KES_ZONE(RENDER, "Create pipeline variant (cache miss)");
        program.pipelines[variant] = CreateMeshPipelineVariant(r, program, variant);
    }

    return program.pipelines[variant];
}

static bool CreateMeshDefaultProgram(Renderer& r)
{
    r.meshDefaultProgram.vertexShader = CreateShaderModuleFromBinary(r, kEngineShaderMeshDefault, ShaderStage::Vertex);
    if (!r.meshDefaultProgram.vertexShader)
        return false;

    r.meshDefaultProgram.pixelShader = CreateShaderModuleFromBinary(r, kEngineShaderMeshDefault, ShaderStage::Pixel);
    if (!r.meshDefaultProgram.pixelShader)
        return false;

    r.meshDefaultProgram.usesFogVarying = false;

    // Eagerly build the common (solid + wireframe, single/double-sided,
    // opaque) variants so the very first frame doesn't stall on pipeline
    // creation; transparent variants are built on first use.
    for (uint32_t variant = 0; variant < kMeshPipelineVariantCount; ++variant)
    {
        const bool transparent = (variant & kVariantBit_Transparent) != 0;
        const bool depthReadOnly = (variant & kVariantBit_DepthReadOnly) != 0;
        if (transparent != depthReadOnly)
            continue; // this combination is never selected by GetOrCreateMeshPipelineVariant
        r.meshDefaultProgram.pipelines[variant] = CreateMeshPipelineVariant(r, r.meshDefaultProgram, variant);
        if (!r.meshDefaultProgram.pipelines[variant])
            return false;
    }
    return true;
}

static bool CreateShaderProgramFromBinaries(Renderer& r, const std::string& vsSource, const std::string& psSource, bool usesFogVarying, GPUShaderProgram& outProgram)
{
    outProgram.vertexShader = CreateShaderModuleFromBinary(r, vsSource, ShaderStage::Vertex);
    if (!outProgram.vertexShader)
        return false;

    outProgram.pixelShader = CreateShaderModuleFromBinary(r, psSource, ShaderStage::Pixel);
    if (!outProgram.pixelShader)
    {
        vkDestroyShaderModule(r.device, outProgram.vertexShader, nullptr);
        outProgram.vertexShader = VK_NULL_HANDLE;
        return false;
    }

    outProgram.usesFogVarying = usesFogVarying;
    return true;
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

    // Stages a .shader asset doesn't provide fall back to the engine's fog-capable mesh program.
    const std::string vs = shader->vertexShaderFile.empty() ? std::string(kEngineShaderMeshFallback) : shader->vertexShaderFile;
    const std::string ps = shader->pixelShaderFile.empty() ? std::string(kEngineShaderMeshFallback) : shader->pixelShaderFile;

    GPUShaderProgram program = {};
    if (!CreateShaderProgramFromBinaries(r, vs, ps, true, program))
    {
        EngineLogf("Failed to create shader program for \"%s\".", shader->name.c_str());
        return nullptr;
    }

    auto inserted = r.gpuShaders.emplace(handle, program);
    return &inserted.first->second;
}

// Sky / blit pipelines (fixed, no vertex input, drawn via gl_VertexIndex)
static VkPipeline CreateFullscreenTrianglePipeline(Renderer& r, VkShaderModule vs, VkShaderModule ps, VkPipelineLayout layout,
    VkRenderPass renderPass, bool depthTestReadOnly, bool hasDepthAttachment)
{
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = hasDepthAttachment ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = (hasDepthAttachment && !depthTestReadOnly) ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        EngineLogf("vkCreateGraphicsPipelines (fullscreen triangle) failed");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

static bool CreateSkyPipeline(Renderer& r)
{
    r.skyVS = CreateShaderModuleFromBinary(r, kEngineShaderSky, ShaderStage::Vertex);
    if (!r.skyVS)
        return false;
    r.skyPS = CreateShaderModuleFromBinary(r, kEngineShaderSky, ShaderStage::Pixel);
    if (!r.skyPS)
        return false;

    // Matches the D3D11 sky draw: solidNoCullRS, opaque blend, depthReadState.
    r.skyPipeline = CreateFullscreenTrianglePipeline(r, r.skyVS, r.skyPS, r.skyPipelineLayout, r.offscreenRenderPass, /*depthTestReadOnly=*/true, /*hasDepthAttachment=*/true);
    return r.skyPipeline != VK_NULL_HANDLE;
}

static bool CreateBlitPipeline(Renderer& r)
{
    r.blitVS = CreateShaderModuleFromBinary(r, kEngineShaderBlit, ShaderStage::Vertex);
    if (!r.blitVS)
        return false;
    r.blitPS = CreateShaderModuleFromBinary(r, kEngineShaderBlit, ShaderStage::Pixel);
    if (!r.blitPS)
        return false;

    // Matches the D3D11 overlay draw: solidRS, opaque blend, no depth buffer bound.
    r.blitPipeline = CreateFullscreenTrianglePipeline(r, r.blitVS, r.blitPS, r.blitPipelineLayout, r.backbufferRenderPass, /*depthTestReadOnly=*/false, /*hasDepthAttachment=*/false);
    return r.blitPipeline != VK_NULL_HANDLE;
}

// UI pipeline (ui.slang): UiVertex input, premultiplied alpha, dynamic scissor.
static bool CreateUiPipeline(Renderer& r)
{
    r.uiVS = CreateShaderModuleFromBinary(r, kEngineShaderUi, ShaderStage::Vertex);
    if (!r.uiVS)
        return false;
    r.uiPS = CreateShaderModuleFromBinary(r, kEngineShaderUi, ShaderStage::Pixel);
    if (!r.uiPS)
        return false;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = r.uiVS;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = r.uiPS;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(UiVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[3] = {};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(UiVertex, x)) };
    attributes[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(offsetof(UiVertex, color)) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(UiVertex, u)) };

    VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = r.uiPipelineLayout;
    pipelineInfo.renderPass = r.backbufferRenderPass;
    pipelineInfo.subpass = 0;

    VK_CHECK_RET(vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &r.uiPipeline), "vkCreateGraphicsPipelines (ui)", false);
    return true;
}

static bool CreateConstantBuffers(Renderer& r)
{
    for (FrameResources& frame : r.frames)
    {
        if (!CreateBuffer(r, sizeof(SceneCBData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, frame.sceneCB, frame.sceneCBMemory))
            return false;
        VK_CHECK_RET(vkMapMemory(r.device, frame.sceneCBMemory, 0, sizeof(SceneCBData), 0, &frame.sceneCBMapped), "vkMapMemory (sceneCB)", false);

        if (!CreateBuffer(r, sizeof(SkyCBData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, frame.skyCB, frame.skyCBMemory))
            return false;
        VK_CHECK_RET(vkMapMemory(r.device, frame.skyCBMemory, 0, sizeof(SkyCBData), 0, &frame.skyCBMapped), "vkMapMemory (skyCB)", false);

        if (!CreateDynamicRingBuffer(r, frame.modelCBRing, sizeof(ModelCBData), kInitialDynamicSlotCapacity))
            return false;
        if (!CreateDynamicRingBuffer(r, frame.materialCBRing, sizeof(MaterialCBData), kInitialDynamicSlotCapacity))
            return false;

        UpdateSceneAndSkyDescriptorBindings(r, frame);
        UpdateDynamicRingDescriptorBinding(r, frame.uboDescriptorSet, 1, frame.modelCBRing);
        UpdateDynamicRingDescriptorBinding(r, frame.uboDescriptorSet, 2, frame.materialCBRing);
    }
    return true;
}

// Textures

static bool UploadPixelsToImage(Renderer& r, VkImage image, uint32_t width, uint32_t height, const void* pixels, VkDeviceSize pixelBytes)
{
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateBuffer(r, pixelBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory))
        return false;

    void* mapped = nullptr;
    vkMapMemory(r.device, stagingMemory, 0, pixelBytes, 0, &mapped);
    std::memcpy(mapped, pixels, static_cast<size_t>(pixelBytes));
    vkUnmapMemory(r.device, stagingMemory);

    VkCommandBuffer cmd = BeginSingleTimeCommands(r);
    if (!cmd)
    {
        DestroyBuffer(r, stagingBuffer, stagingMemory);
        return false;
    }

    TransitionImageLayout(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    TransitionImageLayout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

    EndSingleTimeCommands(r, cmd);
    DestroyBuffer(r, stagingBuffer, stagingMemory);
    return true;
}

static bool CreateWhiteTexture(Renderer& r)
{
    const uint32_t pixel = 0xFFFFFFFFu;

    if (!CreateImage2D(r, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        r.whiteTexture.image, r.whiteTexture.memory))
        return false;

    if (!UploadPixelsToImage(r, r.whiteTexture.image, 1, 1, &pixel, sizeof(pixel)))
        return false;

    r.whiteTexture.view = CreateImageView2D(r, r.whiteTexture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!r.whiteTexture.view)
        return false;

    r.whiteTexture.descriptorSet = AllocateTextureDescriptorSet(r, r.whiteTexture.view);
    return r.whiteTexture.descriptorSet != VK_NULL_HANDLE;
}

static bool CreateTextureFromFile(Renderer& r, const char* path, GPUTexture& outTexture)
{
    KES_ZONE_FN(RENDER);
    KES_ZONE_TEXTF(RENDER, "%s", path ? path : "<null>");
    TextureFileData textureFile = {};
    std::string error;
    if (!LoadTextureFile(path, textureFile, &error))
    {
        EngineLogf("LoadTextureFile failed for %s: %s", path ? path : "<null>", error.c_str());
        return false;
    }

    const VkFormat format = (textureFile.flags & TextureFileFlag_Srgb) != 0 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    if (!CreateImage2D(r, textureFile.width, textureFile.height, format, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        outTexture.image, outTexture.memory))
    {
        EngineLogf("Create texture failed for %s", path ? path : "<null>");
        return false;
    }

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(textureFile.width) * textureFile.height * 4u;
    if (!UploadPixelsToImage(r, outTexture.image, textureFile.width, textureFile.height, textureFile.pixels.data(), pixelBytes))
        return false;

    outTexture.view = CreateImageView2D(r, outTexture.image, format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!outTexture.view)
        return false;

    outTexture.descriptorSet = AllocateTextureDescriptorSet(r, outTexture.view);
    return outTexture.descriptorSet != VK_NULL_HANDLE;
}

static VkDescriptorSet GetMaterialTextureDescriptor(Renderer& r, const MaterialData* material)
{
    if (!material || material->baseTexturePath.empty())
        return r.whiteTexture.descriptorSet;

    const std::string resolvedPath = ResolveAssetPath(material->baseTexturePath.c_str());
    auto it = r.gpuTextures.find(resolvedPath);
    if (it != r.gpuTextures.end())
        return it->second.descriptorSet ? it->second.descriptorSet : r.whiteTexture.descriptorSet;

    GPUTexture texture = {};
    if (!CreateTextureFromFile(r, resolvedPath.c_str(), texture))
    {
        r.gpuTextures.emplace(resolvedPath, GPUTexture{});
        return r.whiteTexture.descriptorSet;
    }

    auto inserted = r.gpuTextures.emplace(resolvedPath, texture);
    return inserted.first->second.descriptorSet ? inserted.first->second.descriptorSet : r.whiteTexture.descriptorSet;
}

// Mesh upload
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
    gpu.indexCount = static_cast<uint32_t>(data->indices.size());

    if (!CreateDeviceLocalBufferWithData(r, vertices.data(), vertices.size() * sizeof(RenderVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gpu.vertexBuffer, gpu.vertexMemory))
        return nullptr;

    if (!data->indices.empty())
    {
        if (!CreateDeviceLocalBufferWithData(r, data->indices.data(), data->indices.size() * sizeof(uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, gpu.indexBuffer, gpu.indexMemory))
        {
            DestroyBuffer(r, gpu.vertexBuffer, gpu.vertexMemory);
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
    gpu.indexCount = static_cast<uint32_t>(source.indices.size());

    if (!CreateDeviceLocalBufferWithData(r, vertices.data(), vertices.size() * sizeof(RenderVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gpu.vertexBuffer, gpu.vertexMemory))
        return nullptr;

    if (!source.indices.empty())
    {
        if (!CreateDeviceLocalBufferWithData(r, source.indices.data(), source.indices.size() * sizeof(uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT, gpu.indexBuffer, gpu.indexMemory))
        {
            DestroyBuffer(r, gpu.vertexBuffer, gpu.vertexMemory);
            return nullptr;
        }
    }

    r.gpuModelMeshes[key] = gpu;
    return &r.gpuModelMeshes[key];
}

// Pure data/math helpers
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

static float3 BuildLightDirection(const Transform& t)
{
    const Vector3 direction = Vector3DirectionFromRotation(t.rotation);
    return { direction.x, direction.y, direction.z };
}


static bool IsTransparentMaterial(const MaterialData* material)
{
    return material && (material->flags & MaterialFlag_Transparent) != 0;
}

static bool IsDoubleSidedMaterial(const MaterialData* material)
{
    return material && (material->flags & MaterialFlag_DoubleSided) != 0;
}

// Asset cache teardown
static void DestroyGPUTexture(Renderer& r, GPUTexture& texture)
{
    if (texture.descriptorSet)
    {
        vkFreeDescriptorSets(r.device, r.descriptorPool, 1, &texture.descriptorSet);
        texture.descriptorSet = VK_NULL_HANDLE;
    }
    if (texture.view) { vkDestroyImageView(r.device, texture.view, nullptr); texture.view = VK_NULL_HANDLE; }
    if (texture.image) { vkDestroyImage(r.device, texture.image, nullptr); texture.image = VK_NULL_HANDLE; }
    if (texture.memory) { vkFreeMemory(r.device, texture.memory, nullptr); texture.memory = VK_NULL_HANDLE; }
}

static void DestroyGPUShaderProgram(Renderer& r, GPUShaderProgram& program)
{
    for (VkPipeline& pipeline : program.pipelines)
    {
        if (pipeline) vkDestroyPipeline(r.device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (program.vertexShader) { vkDestroyShaderModule(r.device, program.vertexShader, nullptr); program.vertexShader = VK_NULL_HANDLE; }
    if (program.pixelShader) { vkDestroyShaderModule(r.device, program.pixelShader, nullptr); program.pixelShader = VK_NULL_HANDLE; }
}

static void DestroyGPUMesh(Renderer& r, GPUMesh& mesh)
{
    DestroyBuffer(r, mesh.vertexBuffer, mesh.vertexMemory);
    DestroyBuffer(r, mesh.indexBuffer, mesh.indexMemory);
}

static void DestroyRendererAssetCaches(Renderer& r)
{
    KES_ZONE_FN(RENDER);
    // Nothing here may be in flight: callers only invoke this outside of an
    // active frame (asset-generation change, or full shutdown).
    for (auto& pair : r.gpuTextures)
        DestroyGPUTexture(r, pair.second);
    r.gpuTextures.clear();

    for (auto& pair : r.gpuMeshes)
        DestroyGPUMesh(r, pair.second);
    r.gpuMeshes.clear();

    for (auto& pair : r.gpuModelMeshes)
        DestroyGPUMesh(r, pair.second);
    r.gpuModelMeshes.clear();

    for (auto& pair : r.gpuShaders)
        DestroyGPUShaderProgram(r, pair.second);
    r.gpuShaders.clear();

    r.gpuModelRenderCaches.clear();
}

// Command pool / synchronization
static bool CreateCommandObjects(Renderer& r)
{
    VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = r.graphicsQueueFamily;
    VK_CHECK_RET(vkCreateCommandPool(r.device, &poolInfo, nullptr, &r.commandPool), "vkCreateCommandPool", false);

    VkCommandPoolCreateInfo transferPoolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    transferPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    transferPoolInfo.queueFamilyIndex = r.graphicsQueueFamily;
    VK_CHECK_RET(vkCreateCommandPool(r.device, &transferPoolInfo, nullptr, &r.transferCommandPool), "vkCreateCommandPool (transfer)", false);

    for (FrameResources& frame : r.frames)
    {
        VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = r.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VK_CHECK_RET(vkAllocateCommandBuffers(r.device, &allocInfo, &frame.commandBuffer), "vkAllocateCommandBuffers", false);

        VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK_RET(vkCreateSemaphore(r.device, &semInfo, nullptr, &frame.imageAvailableSemaphore), "vkCreateSemaphore (imageAvailable)", false);

        VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signaled so the first frames do not block
        VK_CHECK_RET(vkCreateFence(r.device, &fenceInfo, nullptr, &frame.inFlightFence), "vkCreateFence", false);
    }

    r.frameIndex = 0;
    r.commandBuffer = r.frames[0].commandBuffer;
    r.uboDescriptorSet = r.frames[0].uboDescriptorSet;
    r.skyDescriptorSet = r.frames[0].skyDescriptorSet;
    return true;
}

// Init / shutdown / resize
bool RendererInit(Renderer& r, SDL_Window* window, const AppConfig& config)
{
    r.windowWidth = config.windowWidth;
    r.windowHeight = config.windowHeight;
    r.renderWidth = config.renderWidth;
    r.renderHeight = config.renderHeight;
    r.preservePresentationAspect = config.preservePresentationAspect;
    r.preferIntegerPresentationScale = config.preferIntegerPresentationScale;
    r.vsync = config.vsync;

    if (!CreateInstance(r, window)) { RendererShutdown(r); return false; }
    if (!SelectPhysicalDevice(r)) { RendererShutdown(r); return false; }
    if (!CreateLogicalDevice(r)) { RendererShutdown(r); return false; }

    // The sampler must exist before the texture descriptor set layout is
    // created, since it is baked in as an immutable sampler.
    if (!CreateSampler(r)) { RendererShutdown(r); return false; }
    if (!CreateDescriptorSetLayouts(r)) { RendererShutdown(r); return false; }
    if (!CreatePipelineLayouts(r)) { RendererShutdown(r); return false; }
    if (!CreateDescriptorPool(r)) { RendererShutdown(r); return false; }
    if (!CreateUboDescriptorSets(r)) { RendererShutdown(r); return false; }

    if (!CreateRenderTargets(r)) { RendererShutdown(r); return false; }

    if (!CreateMeshDefaultProgram(r)) { RendererShutdown(r); return false; }
    if (!CreateSkyPipeline(r)) { RendererShutdown(r); return false; }
    if (!CreateBlitPipeline(r)) { RendererShutdown(r); return false; }
    if (!CreateUiPipeline(r)) { RendererShutdown(r); return false; }

    if (!CreateConstantBuffers(r)) { RendererShutdown(r); return false; }
    if (!CreateCommandObjects(r)) { RendererShutdown(r); return false; }

#if KES_PROFILE_GPU
    // Records and submits a few one-off command buffers on the graphics queue
    // to calibrate GPU timestamps; the frame command buffer is reset before use.
    r.gpuProfiler = TracyVkContext(r.physicalDevice, r.device, r.graphicsQueue, r.commandBuffer);
    TracyVkContextName(r.gpuProfiler, "Vulkan", 6);
#endif
    if (!CreateWhiteTexture(r)) { RendererShutdown(r); return false; }
    r.uiWhiteDescriptorSet = AllocateTextureDescriptorSet(r, r.whiteTexture.view, r.uiTexSetLayout);
    if (!r.uiWhiteDescriptorSet) { RendererShutdown(r); return false; }

    EngineLogf("Renderer initialized successfully (Vulkan). window=%dx%d render=%dx%d.", r.windowWidth, r.windowHeight, r.renderWidth, r.renderHeight);
    return true;
}

bool RendererResize(Renderer& r, int width, int height)
{
    KES_ZONE_FN(RENDER);
    if (!r.device || width <= 0 || height <= 0)
        return false;

    r.windowWidth = width;
    r.windowHeight = height;

    vkDeviceWaitIdle(r.device);
    DestroyRenderTargets(r);

    if (CreateRenderTargets(r))
        return true;
    r.swapchainNeedsRecreate = true;   // e.g. zero-size window; retried at frame start
    return false;
}

bool RendererSetRenderResolution(Renderer& r, int width, int height)
{
    KES_ZONE_FN(RENDER);
    if (!r.device || width <= 0 || height <= 0)
        return false;
    if (width == r.renderWidth && height == r.renderHeight)
        return true;

    r.renderWidth = width;
    r.renderHeight = height;

    vkDeviceWaitIdle(r.device);
    DestroyRenderTargets(r);
    if (CreateRenderTargets(r))
        return true;
    r.swapchainNeedsRecreate = true;
    return false;
}

void RendererSetVSync(Renderer& r, bool enabled)
{
    if (r.vsync == enabled)
        return;
    r.vsync = enabled;
    r.swapchainNeedsRecreate = true;   // present mode is a swapchain property
}


// Per-draw dynamic constant writes
// Writes only what the shader reads: the two matrices, the skin info and the
// skin matrices that are actually used. ModelCBData is ~8 KB because of the
// 128-matrix skin palette, too much to copy for every static mesh draw.
static VkDeviceSize WriteModelConstants(Renderer& r, const float4x4& wvp, const float4x4& world, const float* skinMatrices, uint32_t skinMatrixCount)
{
    const VkDeviceSize offset = AcquireDynamicSlot(r, r.frames[r.frameIndex].modelCBRing, 1);
    uint8_t* dst = r.frames[r.frameIndex].modelCBRing.mapped + offset;

    float matrix[16];
    MatrixToArrayRowMajor(wvp, matrix);
    std::memcpy(dst + offsetof(ModelCBData, worldViewProj), matrix, sizeof(matrix));
    MatrixToArrayRowMajor(world, matrix);
    std::memcpy(dst + offsetof(ModelCBData, world), matrix, sizeof(matrix));

    if (skinMatrices && skinMatrixCount > 0)
        std::memcpy(dst + offsetof(ModelCBData, skinMatrices), skinMatrices, skinMatrixCount * 16u * sizeof(float));

    const float skinInfo[4] = { skinMatrixCount > 0 ? 1.0f : 0.0f, static_cast<float>(skinMatrixCount), 0.0f, 0.0f };
    std::memcpy(dst + offsetof(ModelCBData, skinInfo), skinInfo, sizeof(skinInfo));
    return offset;
}

static VkDeviceSize WriteMaterialConstants(Renderer& r, const MaterialData* material)
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

    const VkDeviceSize offset = AcquireDynamicSlot(r, r.frames[r.frameIndex].materialCBRing, 2);
    std::memcpy(r.frames[r.frameIndex].materialCBRing.mapped + offset, &matcb, sizeof(matcb));
    return offset;
}

static void BindMeshDrawDescriptors(Renderer& r, VkDeviceSize modelOffset, VkDeviceSize materialOffset, VkDescriptorSet textureSet)
{
    const uint32_t dynamicOffsets[2] = { static_cast<uint32_t>(modelOffset), static_cast<uint32_t>(materialOffset) };
    VkDescriptorSet sets[2] = { r.uboDescriptorSet, textureSet };
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.meshPipelineLayout, 0, 2, sets, 2, dynamicOffsets);
}


// Viewports (negative-height trick: makes Vulkan's screen-space Y and
// depth-range conventions match D3D11's, so the shared camera/projection
// math and every clip-space computation above need no changes at all)


static VkViewport BuildOffscreenViewport(const Renderer& r)
{
    VkViewport vp = {};
    vp.x = 0.0f;
    vp.y = static_cast<float>(r.renderHeight);
    vp.width = static_cast<float>(r.renderWidth);
    vp.height = -static_cast<float>(r.renderHeight);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    return vp;
}

static VkViewport BuildPresentationViewport(const Renderer& r)
{
    VkViewport vp = {};
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

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

    const float topLeftX = (static_cast<float>(r.windowWidth) - targetWidth) * 0.5f;
    const float topLeftY = (static_cast<float>(r.windowHeight) - targetHeight) * 0.5f;

    vp.x = topLeftX;
    vp.y = topLeftY + targetHeight; // flipped
    vp.width = targetWidth;
    vp.height = -targetHeight;
    return vp;
}


// Sky


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
    std::memcpy(r.frames[r.frameIndex].skyCBMapped, &skyCB, sizeof(skyCB));

    vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.skyPipeline);
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.skyPipelineLayout, 0, 1, &r.skyDescriptorSet, 0, nullptr);
    vkCmdDraw(r.commandBuffer, 3, 1, 0, 0);
}


// Frame lifecycle


// Starts recording this frame's command buffer the first time any render
// call is made. Mirrors the fact that D3D11's immediate context has no
// explicit "begin frame" step; Vulkan needs one, so it is hidden here and
// triggered lazily by RendererRenderScene, matching how RendererRender
// always calls RendererRenderScene first.
static void BeginFrameIfNeeded(Renderer& r)
{
    if (r.frameActive)
        return;

    KES_ZONE_FN(RENDER);
    FrameResources& frame = r.frames[r.frameIndex];
    {
        // Time blocked here = CPU waiting on the GPU (or on vsync).
        KES_ZONE(RENDER, "Wait for GPU (in-flight fence)");
        vkWaitForFences(r.device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);

        // Under vsync, running a frame ahead only makes the CPU wait in bursts
        // (alternating ~0 ms and two refresh intervals), and the uneven frame
        // deltas show up as judder. Wait for the previous frame too, so vsync
        // gets one frame in flight; without vsync frames overlap for throughput.
        if (r.vsync)
        {
            const FrameResources& previous = r.frames[(r.frameIndex + kMaxFramesInFlight - 1) % kMaxFramesInFlight];
            vkWaitForFences(r.device, 1, &previous.inFlightFence, VK_TRUE, UINT64_MAX);
        }
    }
    vkResetFences(r.device, 1, &frame.inFlightFence);

    // The fence wait above means every frame before frameNumber - (kMaxFramesInFlight - 1)
    // has finished on the GPU, so UI textures released by then are unused.
    if (!r.retiredUiTextures.empty())
    {
        auto finished = [&r](const RetiredUiTexture& retired)
        { return retired.lastUsableFrame + kMaxFramesInFlight <= r.frameNumber; };
        for (RetiredUiTexture& retired : r.retiredUiTextures)
        {
            if (finished(retired))
                DestroyGPUTexture(r, retired.texture);
        }
        r.retiredUiTextures.erase(std::remove_if(r.retiredUiTextures.begin(), r.retiredUiTextures.end(), finished),
                                  r.retiredUiTextures.end());
    }

    r.commandBuffer = frame.commandBuffer;
    r.uboDescriptorSet = frame.uboDescriptorSet;
    r.skyDescriptorSet = frame.skyDescriptorSet;

    vkResetCommandBuffer(r.commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(r.commandBuffer, &beginInfo);

#if KES_PROFILE_GPU
    {
        // Reads back finished queries; must be outside a render pass.
        KES_ZONE(GPU, "Collect GPU timestamps");
        TracyVkCollect(r.gpuProfiler, r.commandBuffer);
    }
#endif

    r.frames[r.frameIndex].modelCBRing.cursor = 0;
    r.frames[r.frameIndex].materialCBRing.cursor = 0;
    r.frameActive = true;
    r.haveSwapchainImage = false;
}

void RendererRenderScene(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    if (r.assetGeneration != assets.GetGeneration())
    {
        // No pending command buffer may still reference the caches, and this
        // must run before BeginFrameIfNeeded records anything that uses them.
        KES_ZONE(RENDER, "Asset generation changed (device wait idle)");
        vkDeviceWaitIdle(r.device);
        DestroyRendererAssetCaches(r);
        r.assetGeneration = assets.GetGeneration();
    }

    // Swapchain/target recreation (resize, present mode change, out-of-date)
    // must happen before this frame's command buffer starts recording:
    // destroying targets the buffer already references invalidates it.
    if (r.swapchainNeedsRecreate && !r.frameActive)
    {
        KES_ZONE(RENDER, "Recreate swapchain");
        vkDeviceWaitIdle(r.device);
        DestroyRenderTargets(r);
        if (CreateRenderTargets(r))
            r.swapchainNeedsRecreate = false;
    }
    if (!r.offscreenFramebuffer || !r.swapchain)
        return;   // targets unavailable (window minimized); retry next frame

    BeginFrameIfNeeded(r);
    KES_VK_GPU_ZONE(r, "Scene pass");

    float clearColor[4] = {};
    BuildSkyClearColor(sceneObj, clearColor);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = { { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo passBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    passBegin.renderPass = r.offscreenRenderPass;
    passBegin.framebuffer = r.offscreenFramebuffer;
    passBegin.renderArea.offset = { 0, 0 };
    passBegin.renderArea.extent = { static_cast<uint32_t>(r.renderWidth), static_cast<uint32_t>(r.renderHeight) };
    passBegin.clearValueCount = 2;
    passBegin.pClearValues = clearValues;
    vkCmdBeginRenderPass(r.commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport = BuildOffscreenViewport(r);
    vkCmdSetViewport(r.commandBuffer, 0, 1, &viewport);
    const VkRect2D scissor = { { 0, 0 }, { static_cast<uint32_t>(r.renderWidth), static_cast<uint32_t>(r.renderHeight) } };
    vkCmdSetScissor(r.commandBuffer, 0, 1, &scissor);

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

    std::memcpy(r.frames[r.frameIndex].sceneCBMapped, &scb, sizeof(scb));

    // Draw lists live in the renderer so their storage is reused every frame.
    std::vector<DrawItem>& opaqueItems = r.opaqueDrawItems;
    std::vector<DrawItem>& transparentItems = r.transparentDrawItems;
    opaqueItems.clear();
    transparentItems.clear();
    const Vector3 cameraPosition = { camera.x, camera.y, camera.z };

    {
        KES_ZONE(RENDER, "Build draw lists");
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

        // Opaque: group by material (shader, textures and material constants are
        // per material) and draw near-to-far inside a group for early depth reject.
        std::sort(opaqueItems.begin(), opaqueItems.end(),
            [](const DrawItem& a, const DrawItem& b)
            {
                if (a.material != b.material)
                    return a.material < b.material;
                return a.distanceSq < b.distanceSq;
            });

        // Transparent: back-to-front for correct blending.
        std::sort(transparentItems.begin(), transparentItems.end(),
            [](const DrawItem& a, const DrawItem& b)
            {
                return a.distanceSq > b.distanceSq;
            });
        KES_PROFILE_PLOT("Opaque draw items", static_cast<int64_t>(opaqueItems.size()));
        KES_PROFILE_PLOT("Transparent draw items", static_cast<int64_t>(transparentItems.size()));
    }

    // Redundant state filtering across consecutive draws.
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkBuffer boundVertexBuffer = VK_NULL_HANDLE;
    VkBuffer boundIndexBuffer = VK_NULL_HANDLE;
    const MaterialData* cachedMaterial = nullptr;
    VkDeviceSize cachedMaterialOffset = 0;
    VkDescriptorSet cachedTextureSet = VK_NULL_HANDLE;
    bool haveMaterial = false;

    auto drawMesh = [&](const GPUMesh& gpuMesh, VkDeviceSize modelOffset, VkDeviceSize materialOffset, VkDescriptorSet textureSet)
    {
        BindMeshDrawDescriptors(r, modelOffset, materialOffset, textureSet);

        if (gpuMesh.vertexBuffer != boundVertexBuffer)
        {
            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(r.commandBuffer, 0, 1, &gpuMesh.vertexBuffer, &vertexOffset);
            boundVertexBuffer = gpuMesh.vertexBuffer;
        }
        if (gpuMesh.indexBuffer)
        {
            if (gpuMesh.indexBuffer != boundIndexBuffer)
            {
                vkCmdBindIndexBuffer(r.commandBuffer, gpuMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                boundIndexBuffer = gpuMesh.indexBuffer;
            }
            vkCmdDrawIndexed(r.commandBuffer, gpuMesh.indexCount, 1, 0, 0, 0);
        }
        else
        {
            vkCmdDraw(r.commandBuffer, gpuMesh.indexCount, 1, 0, 0);
        }
    };

    auto drawItems = [&](const std::vector<DrawItem>& items, bool transparent)
    {
        for (const DrawItem& item : items)
        {
            const SceneObject& obj = *item.object;
            const KModelData* model = assets.GetModel(obj.model);
            const bool skinned = model && !model->bones.empty() && obj.skinMatrixCount > 0;

            GPUShaderProgram* customProgram = nullptr;
            if (item.material && item.material->shader != 0)
                customProgram = GetOrCompileShaderProgram(r, item.material->shader, assets);

            GPUShaderProgram& activeProgram = customProgram ? *customProgram : r.meshDefaultProgram;
            const bool doubleSided = IsDoubleSidedMaterial(item.material);
            VkPipeline pipeline = GetOrCreateMeshPipelineVariant(r, activeProgram, scene.wireframe, doubleSided, transparent);
            if (!pipeline)
                continue;

            if (pipeline != boundPipeline)
            {
                vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
            }

            // Material constants and texture are identical for every draw of a material.
            if (!haveMaterial || item.material != cachedMaterial)
            {
                cachedMaterial = item.material;
                cachedMaterialOffset = WriteMaterialConstants(r, item.material);
                cachedTextureSet = GetMaterialTextureDescriptor(r, item.material);
                haveMaterial = true;
            }

            const float4x4 objectWorld = TransformToMatrix(obj.transform);

            if (skinned)
            {
                const GPUModelRenderCache* modelRenderCache = GetOrBuildModelRenderCache(r, obj.model, assets);
                if (!modelRenderCache)
                    continue;

                const uint32_t matrixCount = obj.skinMatrixCount > SceneObject::MaxSkinMatrices ? SceneObject::MaxSkinMatrices : obj.skinMatrixCount;
                for (uint32_t meshIndex = 0; meshIndex < static_cast<uint32_t>(model->meshes.size()); ++meshIndex)
                {
                    GPUMesh* gpuMesh = GetOrUploadModelMesh(r, obj.model, meshIndex, assets);
                    if (!gpuMesh)
                        continue;

                    float4x4 meshLocal = {};
                    std::memcpy(&meshLocal, modelRenderCache->meshBindWorldMatrices.data() + static_cast<size_t>(meshIndex) * 16u, sizeof(meshLocal));
                    const float4x4 world = Multiply(meshLocal, objectWorld);
                    const float4x4 wvp = Multiply(world, viewProj);

                    const VkDeviceSize modelOffset = WriteModelConstants(r, wvp, world, obj.skinMatrices, matrixCount);
                    drawMesh(*gpuMesh, modelOffset, cachedMaterialOffset, cachedTextureSet);
                }
            }
            else
            {
                GPUMesh* gpuMesh = GetOrUploadMesh(r, obj.mesh, assets);
                if (!gpuMesh)
                    continue;

                const float4x4 wvp = Multiply(objectWorld, viewProj);
                const VkDeviceSize modelOffset = WriteModelConstants(r, wvp, objectWorld, nullptr, 0);
                drawMesh(*gpuMesh, modelOffset, cachedMaterialOffset, cachedTextureSet);
            }
        }
    };

    {
        KES_ZONE(RENDER, "Draw opaque");
        KES_VK_GPU_ZONE(r, "Opaque");
        drawItems(opaqueItems, false);
    }
    {
        KES_ZONE(RENDER, "Draw transparent");
        KES_VK_GPU_ZONE(r, "Transparent");
        drawItems(transparentItems, true);
    }

    vkCmdEndRenderPass(r.commandBuffer);
}

void RendererBeginBackbufferOverlay(Renderer& r)
{
    // Scene pass skipped this frame (targets missing) or recreation pending:
    // recreation happens at the start of the next RendererRenderScene.
    if (!r.frameActive || r.swapchainNeedsRecreate || !r.swapchain)
    {
        r.haveSwapchainImage = false;
        return;
    }

    VkResult acquireResult = VK_SUCCESS;
    {
        // Can block when every swapchain image is still queued for display (vsync).
        KES_ZONE(RENDER, "vkAcquireNextImageKHR");
        acquireResult = vkAcquireNextImageKHR(r.device, r.swapchain, UINT64_MAX, r.frames[r.frameIndex].imageAvailableSemaphore, VK_NULL_HANDLE, &r.currentSwapchainImageIndex);
    }
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        r.swapchainNeedsRecreate = true;
        r.haveSwapchainImage = false;
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
    {
        EngineLogf("vkAcquireNextImageKHR failed: %d", static_cast<int>(acquireResult));
        r.haveSwapchainImage = false;
        return;
    }

    r.haveSwapchainImage = true;
    KES_VK_GPU_ZONE(r, "Backbuffer blit");

    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    VkClearValue clearValue = {};
    clearValue.color = { { black[0], black[1], black[2], black[3] } };

    VkRenderPassBeginInfo passBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    passBegin.renderPass = r.backbufferRenderPass;
    passBegin.framebuffer = r.backbufferFramebuffers[r.currentSwapchainImageIndex];
    passBegin.renderArea.offset = { 0, 0 };
    passBegin.renderArea.extent = r.swapchainExtent;
    passBegin.clearValueCount = 1;
    passBegin.pClearValues = &clearValue;
    vkCmdBeginRenderPass(r.commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport = BuildPresentationViewport(r);
    vkCmdSetViewport(r.commandBuffer, 0, 1, &viewport);
    const VkRect2D scissor = { { 0, 0 }, r.swapchainExtent }; // no scissor test in the D3D11 path; cover the full window
    vkCmdSetScissor(r.commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.blitPipeline);
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.blitPipelineLayout, 0, 1, &r.offscreenColorDescriptorSet, 0, nullptr);
    vkCmdDraw(r.commandBuffer, 3, 1, 0, 0);

    // Left open for RendererRenderUi; RendererPresent ends it.
    r.backbufferPassOpen = true;
}

static bool EnsureStreamBuffer(Renderer& r, GPUStreamBuffer& stream, VkDeviceSize requiredSize, VkBufferUsageFlags usage)
{
    if (stream.buffer && stream.size >= requiredSize)
        return true;

    // Only this frame's copy is replaced; the GPU finished with it before
    // BeginFrameIfNeeded's fence wait returned.
    if (stream.mapped)
        vkUnmapMemory(r.device, stream.memory);
    stream.mapped = nullptr;
    DestroyBuffer(r, stream.buffer, stream.memory);

    VkDeviceSize size = std::max<VkDeviceSize>(stream.size * 2, 64 * 1024);
    while (size < requiredSize)
        size *= 2;
    stream.size = 0;

    if (!CreateBuffer(r, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stream.buffer, stream.memory))
        return false;
    if (vkMapMemory(r.device, stream.memory, 0, size, 0, &stream.mapped) != VK_SUCCESS)
    {
        stream.mapped = nullptr;
        DestroyBuffer(r, stream.buffer, stream.memory);
        return false;
    }
    stream.size = size;
    return true;
}

static void DestroyStreamBuffer(Renderer& r, GPUStreamBuffer& stream)
{
    if (stream.mapped)
        vkUnmapMemory(r.device, stream.memory);
    stream.mapped = nullptr;
    DestroyBuffer(r, stream.buffer, stream.memory);
    stream.size = 0;
}

UiTextureHandle RendererCreateUiTexture(Renderer& r, int width, int height, const uint8_t* rgbaPixels)
{
    KES_ZONE_FN(RENDER);
    if (!r.device || width <= 0 || height <= 0 || !rgbaPixels)
        return 0;

    GPUTexture texture = {};
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    bool ok = CreateImage2D(r, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                            texture.image, texture.memory);
    ok = ok && UploadPixelsToImage(r, texture.image, w, h, rgbaPixels, static_cast<VkDeviceSize>(w) * h * 4u);
    if (ok)
        texture.view = CreateImageView2D(r, texture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (texture.view)
        texture.descriptorSet = AllocateTextureDescriptorSet(r, texture.view, r.uiTexSetLayout);
    if (!texture.descriptorSet)
    {
        EngineLogf("UI texture creation failed (%dx%d).", width, height);
        DestroyGPUTexture(r, texture);
        return 0;
    }

    const UiTextureHandle handle = r.nextUiTextureHandle++;
    r.uiTextures.emplace(handle, texture);
    return handle;
}

void RendererDestroyUiTexture(Renderer& r, UiTextureHandle handle)
{
    auto it = r.uiTextures.find(handle);
    if (it == r.uiTextures.end())
        return;
    r.retiredUiTextures.push_back({ it->second, r.frameNumber });
    r.uiTextures.erase(it);
}

void RendererRenderUi(Renderer& r, const UiDrawList& drawList)
{
    if (!r.backbufferPassOpen || drawList.Empty() || drawList.vertices.empty() || drawList.indices.empty())
        return;

    KES_ZONE_FN(RENDER);
    KES_VK_GPU_ZONE(r, "UI");

    FrameResources& frame = r.frames[r.frameIndex];
    const VkDeviceSize vertexBytes = drawList.vertices.size() * sizeof(UiVertex);
    const VkDeviceSize indexBytes = drawList.indices.size() * sizeof(uint32_t);
    if (!EnsureStreamBuffer(r, frame.uiVertices, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !EnsureStreamBuffer(r, frame.uiIndices, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return;
    std::memcpy(frame.uiVertices.mapped, drawList.vertices.data(), static_cast<size_t>(vertexBytes));
    std::memcpy(frame.uiIndices.mapped, drawList.indices.data(), static_cast<size_t>(indexBytes));

    const VkExtent2D extent = r.swapchainExtent;
    VkViewport viewport = {};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(r.commandBuffer, 0, 1, &viewport);

    vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.uiPipeline);
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(r.commandBuffer, 0, 1, &frame.uiVertices.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(r.commandBuffer, frame.uiIndices.buffer, 0, VK_INDEX_TYPE_UINT32);

    // Pixels -> clip space; Vulkan clip Y points down like UI coordinates.
    const float scaleOffset[4] = { 2.0f / viewport.width, 2.0f / viewport.height, -1.0f, -1.0f };
    vkCmdPushConstants(r.commandBuffer, r.uiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(scaleOffset), scaleOffset);

    VkDescriptorSet boundSet = VK_NULL_HANDLE;
    for (const UiDrawCommand& command : drawList.commands)
    {
        if (command.indexCount == 0)
            continue;

        VkRect2D scissor = { { 0, 0 }, extent };
        if (command.scissorEnabled)
        {
            const int x0 = std::clamp(command.scissor.x, 0, static_cast<int>(extent.width));
            const int y0 = std::clamp(command.scissor.y, 0, static_cast<int>(extent.height));
            const int x1 = std::clamp(command.scissor.x + command.scissor.width, x0, static_cast<int>(extent.width));
            const int y1 = std::clamp(command.scissor.y + command.scissor.height, y0, static_cast<int>(extent.height));
            if (x1 <= x0 || y1 <= y0)
                continue;
            scissor.offset = { x0, y0 };
            scissor.extent = { static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0) };
        }
        vkCmdSetScissor(r.commandBuffer, 0, 1, &scissor);

        VkDescriptorSet set = r.uiWhiteDescriptorSet;
        if (command.texture != 0)
        {
            auto it = r.uiTextures.find(command.texture);
            if (it != r.uiTextures.end())
                set = it->second.descriptorSet;
        }
        if (set != boundSet)
        {
            vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.uiPipelineLayout, 0, 1, &set, 0, nullptr);
            boundSet = set;
        }

        vkCmdDrawIndexed(r.commandBuffer, command.indexCount, 1, command.firstIndex, 0, 0);
    }
}

void RendererPresent(Renderer& r)
{
    if (!r.frameActive)
        return;

    if (r.backbufferPassOpen)
    {
        vkCmdEndRenderPass(r.commandBuffer);
        r.backbufferPassOpen = false;
    }

    vkEndCommandBuffer(r.commandBuffer);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &r.commandBuffer;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (r.haveSwapchainImage)
    {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &r.frames[r.frameIndex].imageAvailableSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &r.renderFinishedSemaphores[r.currentSwapchainImageIndex];
    }

    {
        KES_ZONE(RENDER, "vkQueueSubmit");
        vkQueueSubmit(r.graphicsQueue, 1, &submitInfo, r.frames[r.frameIndex].inFlightFence);
    }

    if (r.haveSwapchainImage)
    {
        VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &r.renderFinishedSemaphores[r.currentSwapchainImageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &r.swapchain;
        presentInfo.pImageIndices = &r.currentSwapchainImageIndex;

        VkResult presentResult = VK_SUCCESS;
        {
            KES_ZONE(RENDER, "vkQueuePresentKHR");
            presentResult = vkQueuePresentKHR(r.presentQueue, &presentInfo);
        }
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
            r.swapchainNeedsRecreate = true;
        else if (presentResult != VK_SUCCESS)
            EngineLogf("vkQueuePresentKHR failed: %d", static_cast<int>(presentResult));
    }

    r.frameActive = false;
    r.haveSwapchainImage = false;
    r.frameIndex = (r.frameIndex + 1) % kMaxFramesInFlight;
    ++r.frameNumber;
}

void RendererRender(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    RendererRenderScene(r, scene, camera, sceneObj, assets);
    RendererBeginBackbufferOverlay(r);
    RendererPresent(r);
}

void RendererShutdown(Renderer& r)
{
    if (r.device)
        vkDeviceWaitIdle(r.device);

#if KES_PROFILE_GPU
    if (r.gpuProfiler)
    {
        TracyVkDestroy(r.gpuProfiler);
        r.gpuProfiler = nullptr;
    }
#endif

    for (auto& [handle, texture] : r.uiTextures)
        DestroyGPUTexture(r, texture);
    r.uiTextures.clear();
    for (RetiredUiTexture& retired : r.retiredUiTextures)
        DestroyGPUTexture(r, retired.texture);
    r.retiredUiTextures.clear();
    if (r.uiWhiteDescriptorSet)
    {
        vkFreeDescriptorSets(r.device, r.descriptorPool, 1, &r.uiWhiteDescriptorSet);
        r.uiWhiteDescriptorSet = VK_NULL_HANDLE;
    }

    DestroyGPUTexture(r, r.whiteTexture);
    DestroyRendererAssetCaches(r);
    r.assetGeneration = 0;

    for (FrameResources& frame : r.frames)
    {
        DestroyStreamBuffer(r, frame.uiVertices);
        DestroyStreamBuffer(r, frame.uiIndices);
    }

    for (FrameResources& frame : r.frames)
    {
        if (frame.inFlightFence) { vkDestroyFence(r.device, frame.inFlightFence, nullptr); frame.inFlightFence = VK_NULL_HANDLE; }
        if (frame.imageAvailableSemaphore) { vkDestroySemaphore(r.device, frame.imageAvailableSemaphore, nullptr); frame.imageAvailableSemaphore = VK_NULL_HANDLE; }
    }

    if (r.transferCommandPool) { vkDestroyCommandPool(r.device, r.transferCommandPool, nullptr); r.transferCommandPool = VK_NULL_HANDLE; }
    if (r.commandPool)
    {
        vkDestroyCommandPool(r.device, r.commandPool, nullptr);
        r.commandPool = VK_NULL_HANDLE;
        r.commandBuffer = VK_NULL_HANDLE;
        for (FrameResources& frame : r.frames)
            frame.commandBuffer = VK_NULL_HANDLE;
    }

    for (FrameResources& frame : r.frames)
    {
        DestroyDynamicRingBuffer(r, frame.modelCBRing);
        DestroyDynamicRingBuffer(r, frame.materialCBRing);

        if (frame.sceneCBMapped) { vkUnmapMemory(r.device, frame.sceneCBMemory); frame.sceneCBMapped = nullptr; }
        DestroyBuffer(r, frame.sceneCB, frame.sceneCBMemory);
        if (frame.skyCBMapped) { vkUnmapMemory(r.device, frame.skyCBMemory); frame.skyCBMapped = nullptr; }
        DestroyBuffer(r, frame.skyCB, frame.skyCBMemory);
    }

    if (r.uiPipeline) { vkDestroyPipeline(r.device, r.uiPipeline, nullptr); r.uiPipeline = VK_NULL_HANDLE; }
    if (r.uiPS) { vkDestroyShaderModule(r.device, r.uiPS, nullptr); r.uiPS = VK_NULL_HANDLE; }
    if (r.uiVS) { vkDestroyShaderModule(r.device, r.uiVS, nullptr); r.uiVS = VK_NULL_HANDLE; }

    if (r.blitPipeline) { vkDestroyPipeline(r.device, r.blitPipeline, nullptr); r.blitPipeline = VK_NULL_HANDLE; }
    if (r.blitPS) { vkDestroyShaderModule(r.device, r.blitPS, nullptr); r.blitPS = VK_NULL_HANDLE; }
    if (r.blitVS) { vkDestroyShaderModule(r.device, r.blitVS, nullptr); r.blitVS = VK_NULL_HANDLE; }

    if (r.skyPipeline) { vkDestroyPipeline(r.device, r.skyPipeline, nullptr); r.skyPipeline = VK_NULL_HANDLE; }
    if (r.skyPS) { vkDestroyShaderModule(r.device, r.skyPS, nullptr); r.skyPS = VK_NULL_HANDLE; }
    if (r.skyVS) { vkDestroyShaderModule(r.device, r.skyVS, nullptr); r.skyVS = VK_NULL_HANDLE; }

    DestroyGPUShaderProgram(r, r.meshDefaultProgram);

    DestroyRenderTargets(r);
    if (r.offscreenRenderPass) { vkDestroyRenderPass(r.device, r.offscreenRenderPass, nullptr); r.offscreenRenderPass = VK_NULL_HANDLE; }
    if (r.backbufferRenderPass) { vkDestroyRenderPass(r.device, r.backbufferRenderPass, nullptr); r.backbufferRenderPass = VK_NULL_HANDLE; }

    // Descriptor sets allocated from a FREE_DESCRIPTOR_SET_BIT pool are
    // released with the pool itself; explicit frees above already handled
    // the rest, so just tear down the pool and layouts.
    if (r.descriptorPool) { vkDestroyDescriptorPool(r.device, r.descriptorPool, nullptr); r.descriptorPool = VK_NULL_HANDLE; }
    if (r.meshPipelineLayout) { vkDestroyPipelineLayout(r.device, r.meshPipelineLayout, nullptr); r.meshPipelineLayout = VK_NULL_HANDLE; }
    if (r.skyPipelineLayout) { vkDestroyPipelineLayout(r.device, r.skyPipelineLayout, nullptr); r.skyPipelineLayout = VK_NULL_HANDLE; }
    if (r.blitPipelineLayout) { vkDestroyPipelineLayout(r.device, r.blitPipelineLayout, nullptr); r.blitPipelineLayout = VK_NULL_HANDLE; }
    if (r.uiPipelineLayout) { vkDestroyPipelineLayout(r.device, r.uiPipelineLayout, nullptr); r.uiPipelineLayout = VK_NULL_HANDLE; }
    if (r.uiTexSetLayout) { vkDestroyDescriptorSetLayout(r.device, r.uiTexSetLayout, nullptr); r.uiTexSetLayout = VK_NULL_HANDLE; }
    if (r.uboSetLayout) { vkDestroyDescriptorSetLayout(r.device, r.uboSetLayout, nullptr); r.uboSetLayout = VK_NULL_HANDLE; }
    if (r.texSetLayout) { vkDestroyDescriptorSetLayout(r.device, r.texSetLayout, nullptr); r.texSetLayout = VK_NULL_HANDLE; }
    if (r.skySetLayout) { vkDestroyDescriptorSetLayout(r.device, r.skySetLayout, nullptr); r.skySetLayout = VK_NULL_HANDLE; }

    if (r.pointSampler) { vkDestroySampler(r.device, r.pointSampler, nullptr); r.pointSampler = VK_NULL_HANDLE; }
    if (r.linearSampler) { vkDestroySampler(r.device, r.linearSampler, nullptr); r.linearSampler = VK_NULL_HANDLE; }

    if (r.device) { vkDestroyDevice(r.device, nullptr); r.device = VK_NULL_HANDLE; }

    if (r.surface) { SDL_Vulkan_DestroySurface(r.instance, r.surface, nullptr); r.surface = VK_NULL_HANDLE; }

#ifdef _DEBUG
    if (r.debugMessenger)
    {
        auto destroyDebugUtils = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(r.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyDebugUtils)
            destroyDebugUtils(r.instance, r.debugMessenger, nullptr);
        r.debugMessenger = VK_NULL_HANDLE;
    }
#endif

    if (r.instance) { vkDestroyInstance(r.instance, nullptr); r.instance = VK_NULL_HANDLE; }
}

} // namespace kes::vk

namespace
{
    class VulkanRenderer final : public IRenderer
    {
    public:
        ~VulkanRenderer() override { Shutdown(); }

        RendererBackend GetBackend() const override { return RendererBackend::Vulkan; }

        bool Init(SDL_Window* window, const AppConfig& config) override
        {
            // RendererInit releases everything it created on failure.
            m_initialized = kes::vk::RendererInit(m_state, window, config);
            if (!m_initialized)
                m_state = {};
            return m_initialized;
        }

        void Shutdown() override
        {
            if (!m_initialized)
                return;
            kes::vk::RendererShutdown(m_state);
            m_state = {};
            m_initialized = false;
        }

        bool Resize(int width, int height) override { return kes::vk::RendererResize(m_state, width, height); }
        bool SetRenderResolution(int width, int height) override { return kes::vk::RendererSetRenderResolution(m_state, width, height); }
        void SetVSync(bool enabled) override { kes::vk::RendererSetVSync(m_state, enabled); }
        bool IsVSyncEnabled() const override { return m_state.vsync; }

        void RenderScene(const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets) override
        {
            kes::vk::RendererRenderScene(m_state, scene, camera, sceneObj, assets);
        }

        void BeginBackbufferOverlay() override { kes::vk::RendererBeginBackbufferOverlay(m_state); }

        UiTextureHandle CreateUiTexture(int width, int height, const uint8_t* rgbaPixels) override
        {
            return kes::vk::RendererCreateUiTexture(m_state, width, height, rgbaPixels);
        }
        void DestroyUiTexture(UiTextureHandle texture) override { kes::vk::RendererDestroyUiTexture(m_state, texture); }
        void RenderUi(const UiDrawList& drawList) override { kes::vk::RendererRenderUi(m_state, drawList); }

        void Present() override { kes::vk::RendererPresent(m_state); }

    private:
        kes::vk::Renderer m_state = {};
        bool m_initialized = false;
    };
}

std::unique_ptr<IRenderer> CreateVulkanRenderer()
{
    return std::make_unique<VulkanRenderer>();
}

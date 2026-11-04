#ifndef NOMINMAX
#define NOMINMAX   // file_paths.h includes <windows.h>
#endif
#include "vulkan_render.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cstddef>
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

using render::Matrix4;

static constexpr VkFormat kSceneColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct RenderVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
    uint16_t boneIndices[4];
    float boneWeights[4];
};

static void DestroyRendererAssetCaches(Renderer& r);
static const GPUModelRenderCache* GetOrBuildModelRenderCache(Renderer& r, ModelHandle handle, const AssetSystem& assets);
static void DestroyGPUShaderProgram(Renderer& r, GPUShaderProgram& program);
static void DestroyGPUTexture(Renderer& r, GPUTexture& texture);

// Shaders are authored once in Slang (engine/shaders/*.slang) and compiled to
// SPIR-V at build time (cmake/KesShaders.cmake). Nothing is compiled at runtime.

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

static const char* VkResultName(VkResult result)
{
    switch (result)
    {
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    default: return "unknown VkResult";
    }
}

// Errors here don't recover: the swapchain's own out-of-date results are
// handled by the callers before this.
static bool CheckGpuResult(Renderer& r, VkResult result, const char* call)
{
    if (result >= VK_SUCCESS)
        return true;
    if (r.fatalError.empty())
        r.fatalError = std::string(call) + " failed: " + VkResultName(result) + " (" + std::to_string(static_cast<int>(result)) + ")";
    return false;
}

static void EndSingleTimeCommands(Renderer& r, VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    CheckGpuResult(r, vkQueueSubmit(r.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit (upload)");
    CheckGpuResult(r, vkQueueWaitIdle(r.graphicsQueue), "vkQueueWaitIdle"); // asset loading is not perf critical here; mirrors D3D11's synchronous immediate-context uploads

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

// Images

struct ImageDesc
{
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;
    uint32_t layers = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateFlags flags = 0;
};

static bool CreateImage(Renderer& r, const ImageDesc& desc, VkImage& outImage, VkDeviceMemory& outMemory)
{
    VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.flags = desc.flags;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { desc.width, desc.height, 1 };
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = desc.layers;
    imageInfo.format = desc.format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = desc.usage;
    imageInfo.samples = desc.samples;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK_RET(vkCreateImage(r.device, &imageInfo, nullptr, &outImage), "vkCreateImage", false);

    VkMemoryRequirements memReq = {};
    vkGetImageMemoryRequirements(r.device, outImage, &memReq);

    const uint32_t memoryType = FindMemoryType(r, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (memoryType == UINT32_MAX || vkAllocateMemory(r.device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS)
    {
        vkDestroyImage(r.device, outImage, nullptr);
        outImage = VK_NULL_HANDLE;
        EngineLogf("Image memory allocation failed (%ux%u x%u)", desc.width, desc.height, desc.layers);
        return false;
    }

    vkBindImageMemory(r.device, outImage, outMemory, 0);
    return true;
}

static VkImageView CreateImageView(Renderer& r, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                                   VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D, uint32_t baseLayer = 0, uint32_t layerCount = 1,
                                   uint32_t levelCount = 1)
{
    VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = image;
    viewInfo.viewType = type;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = levelCount;
    viewInfo.subresourceRange.baseArrayLayer = baseLayer;
    viewInfo.subresourceRange.layerCount = layerCount;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(r.device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        EngineLogf("vkCreateImageView failed");
        return VK_NULL_HANDLE;
    }
    return view;
}

// Coarse but correct barrier for one-off transitions (uploads, creation).
static void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                  VkImageAspectFlags aspect, uint32_t levels = 1, uint32_t layers = 1)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = levels;
    barrier.subresourceRange.layerCount = layers;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

struct UploadRegion
{
    uint32_t level = 0;
    uint32_t layer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkDeviceSize offset = 0;
};

// Copies `bytes` into the image's regions and leaves it in SHADER_READ_ONLY_OPTIMAL.
static bool UploadImage(Renderer& r, VkImage image, const void* bytes, VkDeviceSize size, const std::vector<UploadRegion>& regions,
                        uint32_t levels, uint32_t layers)
{
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (!CreateBuffer(r, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory))
        return false;

    void* mapped = nullptr;
    vkMapMemory(r.device, stagingMemory, 0, size, 0, &mapped);
    std::memcpy(mapped, bytes, static_cast<size_t>(size));
    vkUnmapMemory(r.device, stagingMemory);

    VkCommandBuffer cmd = BeginSingleTimeCommands(r);
    if (!cmd)
    {
        DestroyBuffer(r, stagingBuffer, stagingMemory);
        return false;
    }

    TransitionImageLayout(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, levels, layers);
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(regions.size());
    for (const UploadRegion& region : regions)
    {
        VkBufferImageCopy copy = {};
        copy.bufferOffset = region.offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = region.level;
        copy.imageSubresource.baseArrayLayer = region.layer;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { region.width, region.height, 1 };
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
    TransitionImageLayout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, levels, layers);

    EndSingleTimeCommands(r, cmd);
    DestroyBuffer(r, stagingBuffer, stagingMemory);
    return true;
}

// Uploads an RGBA8 mip chain as a sampled 2D texture.
static bool CreateTexture2D(Renderer& r, const std::vector<render::MipLevel>& levels, VkFormat format, GPUTexture& out)
{
    if (levels.empty())
        return false;
    std::vector<uint8_t> bytes;
    std::vector<UploadRegion> regions;
    for (uint32_t level = 0; level < static_cast<uint32_t>(levels.size()); ++level)
    {
        regions.push_back({ level, 0, levels[level].width, levels[level].height, bytes.size() });
        bytes.insert(bytes.end(), levels[level].pixels.begin(), levels[level].pixels.end());
    }

    ImageDesc desc;
    desc.width = levels[0].width;
    desc.height = levels[0].height;
    desc.mipLevels = static_cast<uint32_t>(levels.size());
    desc.format = format;
    desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!CreateImage(r, desc, out.image, out.memory) ||
        !UploadImage(r, out.image, bytes.data(), bytes.size(), regions, desc.mipLevels, 1))
    {
        DestroyGPUTexture(r, out);
        return false;
    }
    out.view = CreateImageView(r, out.image, format, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 0, 1, desc.mipLevels);
    if (!out.view)
    {
        DestroyGPUTexture(r, out);
        return false;
    }
    return true;
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

#ifndef NDEBUG
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

#ifndef NDEBUG
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

#ifndef NDEBUG
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

    VkPhysicalDeviceFeatures supported = {};
    vkGetPhysicalDeviceFeatures(r.physicalDevice, &supported);
    VkPhysicalDeviceFeatures features = {};
    features.fillModeNonSolid = supported.fillModeNonSolid; // wireframe rasterizer variants (F1 debug toggle)
    features.samplerAnisotropy = supported.samplerAnisotropy;
    r.anisotropySupported = supported.samplerAnisotropy == VK_TRUE;

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

    // UNORM: the tonemap encodes sRGB itself, and the UI blends in display space.
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

static VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, int windowWidth, int windowHeight)
{
    if (caps.currentExtent.width != UINT32_MAX)
        return caps.currentExtent;

    VkExtent2D extent = { static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight) };
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
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
        r.swapchainImageViews[i] = CreateImageView(r, r.swapchainImages[i], r.swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT);
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

// Formats and render passes

static bool ChooseDepthFormats(Renderer& r)
{
    const VkFormat candidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT };
    for (VkFormat format : candidates)
    {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(r.physicalDevice, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            r.depthFormat = format;
            break;
        }
    }
    if (!r.depthFormat)
    {
        EngineLogf("No suitable depth/stencil format found");
        return false;
    }

    // Shadow maps: depth attachment + sampled, preferably with linear (PCF) filtering.
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (bool requireLinear : { true, false })
    {
        for (VkFormat format : { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM })
        {
            VkFormatProperties props = {};
            vkGetPhysicalDeviceFormatProperties(r.physicalDevice, format, &props);
            const bool linear = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
            if ((props.optimalTilingFeatures & needed) == needed && (linear || !requireLinear))
            {
                r.shadowFormat = format;
                r.shadowLinearFilter = linear;
                return true;
            }
        }
    }
    EngineLogf("No sampleable depth format for shadow maps found");
    return false;
}

static VkSampleCountFlagBits ChooseMsaaSamples(Renderer& r, int requested)
{
    const VkSampleCountFlags supported = r.physicalDeviceProperties.limits.framebufferColorSampleCounts &
                                         r.physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    for (int samples = std::clamp(requested, 1, 8); samples > 1; samples /= 2)
    {
        if (supported & static_cast<VkSampleCountFlags>(samples))
            return static_cast<VkSampleCountFlagBits>(samples);
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

// HDR color (+ depth); with MSAA the color resolves into a single-sample
// attachment that the tonemap samples.
static bool CreateSceneRenderPass(Renderer& r)
{
    const bool msaa = r.msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription attachments[3] = {};

    attachments[0].format = kSceneColorFormat;
    attachments[0].samples = r.msaaSamples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = r.depthFormat;
    attachments[1].samples = r.msaaSamples;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    attachments[2].format = kSceneColorFormat;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resolveRef = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = msaa ? &resolveRef : nullptr;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    // Previous frame's tonemap read of the color target (WAR) and its depth
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

    VkRenderPassCreateInfo passInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = msaa ? 3u : 2u;
    passInfo.pAttachments = attachments;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;

    VK_CHECK_RET(vkCreateRenderPass(r.device, &passInfo, nullptr, &r.sceneRenderPass), "vkCreateRenderPass (scene)", false);
    return true;
}

// Depth only. Shadow images live in DEPTH_STENCIL_READ_ONLY_OPTIMAL between
// passes, so layers not rendered this frame stay valid for sampling.
static bool CreateShadowRenderPass(Renderer& r)
{
    VkAttachmentDescription depth = {};
    depth.format = r.shadowFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo passInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &depth;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = dependencies;
    VK_CHECK_RET(vkCreateRenderPass(r.device, &passInfo, nullptr, &r.shadowRenderPass), "vkCreateRenderPass (shadow)", false);
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

// Samplers

static bool CreateMaterialSampler(Renderer& r)
{
    if (r.materialSampler)
    {
        vkDestroySampler(r.device, r.materialSampler, nullptr);
        r.materialSampler = VK_NULL_HANDLE;
    }
    const float anisotropy = std::min(static_cast<float>(std::clamp(r.options.anisotropy, 1, 16)),
                                      r.physicalDeviceProperties.limits.maxSamplerAnisotropy);
    VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.anisotropyEnable = (r.anisotropySupported && anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = info.anisotropyEnable ? anisotropy : 1.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK_RET(vkCreateSampler(r.device, &info, nullptr, &r.materialSampler), "vkCreateSampler (material)", false);
    return true;
}

static bool CreateSamplers(Renderer& r)
{
    if (!CreateMaterialSampler(r))
        return false;

    // Environment, tonemap and UI: bilinear, clamped.
    VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK_RET(vkCreateSampler(r.device, &info, nullptr, &r.linearSampler), "vkCreateSampler (linear)", false);

    // PCF: depth comparison, outside the map counts as lit.
    VkSamplerCreateInfo shadow = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    shadow.magFilter = r.shadowLinearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    shadow.minFilter = shadow.magFilter;
    shadow.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    shadow.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shadow.compareEnable = VK_TRUE;
    shadow.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadow.maxLod = 0.0f;
    VK_CHECK_RET(vkCreateSampler(r.device, &shadow, nullptr, &r.shadowSampler), "vkCreateSampler (shadow)", false);
    return true;
}

// Descriptor / pipeline layouts

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

    // set 1: material textures. The sampler is written per set (not
    // immutable), so changing anisotropy only rewrites the sets.
    VkDescriptorSetLayoutBinding materialBindings[render::kMaterialTextureCount] = {};
    for (uint32_t i = 0; i < render::kMaterialTextureCount; ++i)
    {
        materialBindings[i].binding = i;
        materialBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        materialBindings[i].descriptorCount = 1;
        materialBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo materialLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    materialLayoutInfo.bindingCount = render::kMaterialTextureCount;
    materialLayoutInfo.pBindings = materialBindings;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &materialLayoutInfo, nullptr, &r.materialSetLayout), "vkCreateDescriptorSetLayout (material)", false);

    // set 2: environment cube, cascade + local shadow arrays, linear + compare samplers.
    VkDescriptorSetLayoutBinding globalBindings[5] = {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        globalBindings[i].binding = i;
        globalBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        globalBindings[i].descriptorCount = 1;
        globalBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    globalBindings[3].binding = 3;
    globalBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    globalBindings[3].descriptorCount = 1;
    globalBindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    globalBindings[3].pImmutableSamplers = &r.linearSampler;
    globalBindings[4].binding = 4;
    globalBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    globalBindings[4].descriptorCount = 1;
    globalBindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    globalBindings[4].pImmutableSamplers = &r.shadowSampler;
    VkDescriptorSetLayoutCreateInfo globalLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    globalLayoutInfo.bindingCount = 5;
    globalLayoutInfo.pBindings = globalBindings;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &globalLayoutInfo, nullptr, &r.globalSetLayout), "vkCreateDescriptorSetLayout (global)", false);

    // Tonemap and UI: one combined image sampler, linear immutable sampler.
    VkDescriptorSetLayoutBinding texBinding = {};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    texBinding.pImmutableSamplers = &r.linearSampler;
    VkDescriptorSetLayoutCreateInfo texLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    texLayoutInfo.bindingCount = 1;
    texLayoutInfo.pBindings = &texBinding;
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &texLayoutInfo, nullptr, &r.texSetLayout), "vkCreateDescriptorSetLayout (tex)", false);
    VK_CHECK_RET(vkCreateDescriptorSetLayout(r.device, &texLayoutInfo, nullptr, &r.uiTexSetLayout), "vkCreateDescriptorSetLayout (ui tex)", false);

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
    VkDescriptorSetLayout meshSets[3] = { r.uboSetLayout, r.materialSetLayout, r.globalSetLayout };
    VkPipelineLayoutCreateInfo meshLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    meshLayoutInfo.setLayoutCount = 3;
    meshLayoutInfo.pSetLayouts = meshSets;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &meshLayoutInfo, nullptr, &r.meshPipelineLayout), "vkCreatePipelineLayout (mesh)", false);

    VkPipelineLayoutCreateInfo skyLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    skyLayoutInfo.setLayoutCount = 1;
    skyLayoutInfo.pSetLayouts = &r.skySetLayout;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &skyLayoutInfo, nullptr, &r.skyPipelineLayout), "vkCreatePipelineLayout (sky)", false);

    VkPushConstantRange tonemapPush = {};
    tonemapPush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    tonemapPush.size = sizeof(render::TonemapCBData);
    VkPipelineLayoutCreateInfo tonemapLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    tonemapLayoutInfo.setLayoutCount = 1;
    tonemapLayoutInfo.pSetLayouts = &r.texSetLayout;
    tonemapLayoutInfo.pushConstantRangeCount = 1;
    tonemapLayoutInfo.pPushConstantRanges = &tonemapPush;
    VK_CHECK_RET(vkCreatePipelineLayout(r.device, &tonemapLayoutInfo, nullptr, &r.tonemapPipelineLayout), "vkCreatePipelineLayout (tonemap)", false);

    VkPushConstantRange uiPushRange = {};
    uiPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
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
    // Material sets (5 images each) are allocated per unique texture set, UI
    // sets per UI texture, the rest a few per frame in flight.
    VkDescriptorPoolSize poolSizes[5] = {};
    poolSizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 };
    poolSizes[1] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8 };
    poolSizes[2] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384 };
    poolSizes[3] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 };
    poolSizes[4] = { VK_DESCRIPTOR_TYPE_SAMPLER, 16 };

    VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 4096 + 16;
    poolInfo.poolSizeCount = 5;
    poolInfo.pPoolSizes = poolSizes;

    VK_CHECK_RET(vkCreateDescriptorPool(r.device, &poolInfo, nullptr, &r.descriptorPool), "vkCreateDescriptorPool", false);
    return true;
}

static VkDescriptorSet AllocateDescriptorSet(Renderer& r, VkDescriptorSetLayout layout)
{
    VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = r.descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(r.device, &allocInfo, &set) != VK_SUCCESS)
    {
        EngineLogf("vkAllocateDescriptorSets failed");
        return VK_NULL_HANDLE;
    }
    return set;
}

// One combined image sampler at binding 0 (tonemap / UI layouts, linear immutable sampler).
static VkDescriptorSet AllocateTextureDescriptorSet(Renderer& r, VkImageView view, VkDescriptorSetLayout layout)
{
    VkDescriptorSet set = AllocateDescriptorSet(r, layout);
    if (!set)
        return VK_NULL_HANDLE;

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = view;
    imageInfo.sampler = r.linearSampler; // ignored (immutable sampler), set for clarity

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
        frame.uboDescriptorSet = AllocateDescriptorSet(r, r.uboSetLayout);
        frame.skyDescriptorSet = AllocateDescriptorSet(r, r.skySetLayout);
        if (!frame.uboDescriptorSet || !frame.skyDescriptorSet)
            return false;
    }
    r.globalDescriptorSet = AllocateDescriptorSet(r, r.globalSetLayout);
    return r.globalDescriptorSet != VK_NULL_HANDLE;
}

static void UpdateSceneAndSkyDescriptorBindings(Renderer& r, const FrameResources& frame)
{
    VkDescriptorBufferInfo sceneInfo = { frame.sceneCB, 0, sizeof(render::SceneCBData) };
    VkDescriptorBufferInfo skyInfo = { frame.skyCB, 0, sizeof(render::SkyCBData) };

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

// Points set 2 at the current environment and shadow images. Only called
// while no frame using the set can be in flight (after vkDeviceWaitIdle).
static void UpdateGlobalDescriptorSet(Renderer& r)
{
    const GPUShadowArray& cascades = r.cascadeShadows.arrayView ? r.cascadeShadows : r.dummyShadows;
    const GPUShadowArray& locals = r.localShadows.arrayView ? r.localShadows : r.dummyShadows;
    VkDescriptorImageInfo images[3] = {};
    images[0] = { VK_NULL_HANDLE, r.environmentMap.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    images[1] = { VK_NULL_HANDLE, cascades.arrayView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    images[2] = { VK_NULL_HANDLE, locals.arrayView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[i].dstSet = r.globalDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(r.device, 3, writes, 0, nullptr);
    r.globalSetDirty = false;
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

    const uint32_t cursor = ring.cursor;
    DestroyDynamicRingBuffer(r, ring);
    ring.buffer = newBuffer;
    ring.memory = newMemory;
    ring.mapped = newMapped;
    ring.slotCapacity = newCapacity;
    ring.cursor = cursor;

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

// Render targets

static void DestroySceneTargets(Renderer& r)
{
    if (r.tonemapDescriptorSet)
    {
        vkFreeDescriptorSets(r.device, r.descriptorPool, 1, &r.tonemapDescriptorSet);
        r.tonemapDescriptorSet = VK_NULL_HANDLE;
    }
    if (r.sceneFramebuffer) { vkDestroyFramebuffer(r.device, r.sceneFramebuffer, nullptr); r.sceneFramebuffer = VK_NULL_HANDLE; }
    const struct { VkImageView* view; VkImage* image; VkDeviceMemory* memory; } targets[] = {
        { &r.sceneColorView, &r.sceneColorImage, &r.sceneColorMemory },
        { &r.sceneColorMSView, &r.sceneColorMSImage, &r.sceneColorMSMemory },
        { &r.depthView, &r.depthImage, &r.depthMemory } };
    for (const auto& target : targets)
    {
        if (*target.view) { vkDestroyImageView(r.device, *target.view, nullptr); *target.view = VK_NULL_HANDLE; }
        if (*target.image) { vkDestroyImage(r.device, *target.image, nullptr); *target.image = VK_NULL_HANDLE; }
        if (*target.memory) { vkFreeMemory(r.device, *target.memory, nullptr); *target.memory = VK_NULL_HANDLE; }
    }
}

static bool CreateSceneTargets(Renderer& r)
{
    const bool msaa = r.msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    ImageDesc color;
    color.width = static_cast<uint32_t>(std::max(r.renderWidth, 1));
    color.height = static_cast<uint32_t>(std::max(r.renderHeight, 1));
    color.format = kSceneColorFormat;
    color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!CreateImage(r, color, r.sceneColorImage, r.sceneColorMemory))
        return false;
    r.sceneColorView = CreateImageView(r, r.sceneColorImage, kSceneColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);

    if (msaa)
    {
        ImageDesc ms = color;
        ms.samples = r.msaaSamples;
        ms.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        if (!CreateImage(r, ms, r.sceneColorMSImage, r.sceneColorMSMemory))
            return false;
        r.sceneColorMSView = CreateImageView(r, r.sceneColorMSImage, kSceneColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    ImageDesc depth = color;
    depth.format = r.depthFormat;
    depth.samples = r.msaaSamples;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!CreateImage(r, depth, r.depthImage, r.depthMemory))
        return false;
    VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (r.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || r.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
        depthAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    r.depthView = CreateImageView(r, r.depthImage, r.depthFormat, depthAspect);
    if (!r.sceneColorView || !r.depthView || (msaa && !r.sceneColorMSView))
        return false;

    VkImageView attachments[3] = { msaa ? r.sceneColorMSView : r.sceneColorView, r.depthView, r.sceneColorView };
    VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbInfo.renderPass = r.sceneRenderPass;
    fbInfo.attachmentCount = msaa ? 3u : 2u;
    fbInfo.pAttachments = attachments;
    fbInfo.width = color.width;
    fbInfo.height = color.height;
    fbInfo.layers = 1;
    VK_CHECK_RET(vkCreateFramebuffer(r.device, &fbInfo, nullptr, &r.sceneFramebuffer), "vkCreateFramebuffer (scene)", false);

    r.tonemapDescriptorSet = AllocateTextureDescriptorSet(r, r.sceneColorView, r.texSetLayout);
    return r.tonemapDescriptorSet != VK_NULL_HANDLE;
}

static void DestroyRenderTargets(Renderer& r)
{
    DestroySceneTargets(r);
    DestroySwapchainObjects(r);
}

static bool CreateRenderTargets(Renderer& r)
{
    if (!CreateSwapchain(r))
        return false;
    if (!r.backbufferRenderPass && !CreateBackbufferRenderPass(r))
        return false;
    if (!CreateSceneTargets(r))
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

// Shadow maps

static void DestroyShadowArray(Renderer& r, GPUShadowArray& shadows)
{
    for (VkFramebuffer framebuffer : shadows.framebuffers)
        vkDestroyFramebuffer(r.device, framebuffer, nullptr);
    shadows.framebuffers.clear();
    for (VkImageView view : shadows.layerViews)
        vkDestroyImageView(r.device, view, nullptr);
    shadows.layerViews.clear();
    if (shadows.arrayView) { vkDestroyImageView(r.device, shadows.arrayView, nullptr); shadows.arrayView = VK_NULL_HANDLE; }
    if (shadows.image) { vkDestroyImage(r.device, shadows.image, nullptr); shadows.image = VK_NULL_HANDLE; }
    if (shadows.memory) { vkFreeMemory(r.device, shadows.memory, nullptr); shadows.memory = VK_NULL_HANDLE; }
    shadows.resolution = 0;
    shadows.layers = 0;
}

static bool CreateShadowArray(Renderer& r, GPUShadowArray& shadows, int layers, int resolution, bool renderable)
{
    DestroyShadowArray(r, shadows);
    if (layers <= 0 || resolution <= 0)
        return true;

    ImageDesc desc;
    desc.width = desc.height = static_cast<uint32_t>(resolution);
    desc.layers = static_cast<uint32_t>(layers);
    desc.format = r.shadowFormat;
    desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!CreateImage(r, desc, shadows.image, shadows.memory))
        return false;

    // Every layer starts in the layout the passes and the sampler expect.
    VkCommandBuffer cmd = BeginSingleTimeCommands(r);
    if (!cmd)
        return false;
    TransitionImageLayout(cmd, shadows.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                          VK_IMAGE_ASPECT_DEPTH_BIT, 1, desc.layers);
    EndSingleTimeCommands(r, cmd);

    shadows.arrayView = CreateImageView(r, shadows.image, r.shadowFormat, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, desc.layers);
    if (!shadows.arrayView)
        return false;
    shadows.resolution = resolution;
    shadows.layers = layers;
    if (!renderable)
        return true;

    for (uint32_t layer = 0; layer < desc.layers; ++layer)
    {
        VkImageView view = CreateImageView(r, shadows.image, r.shadowFormat, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D, layer, 1);
        if (!view)
            return false;
        shadows.layerViews.push_back(view);

        VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbInfo.renderPass = r.shadowRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &view;
        fbInfo.width = desc.width;
        fbInfo.height = desc.height;
        fbInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VK_CHECK_RET(vkCreateFramebuffer(r.device, &fbInfo, nullptr, &framebuffer), "vkCreateFramebuffer (shadow)", false);
        shadows.framebuffers.push_back(framebuffer);
    }
    return true;
}

static bool CreateShadowTargets(Renderer& r)
{
    const render::ShadowConfig config = render::GetShadowConfig(r.options.shadowQuality);
    const bool ok = CreateShadowArray(r, r.cascadeShadows, config.cascadeCount, config.cascadeResolution, true) &&
                    CreateShadowArray(r, r.localShadows, config.localSlices, config.localResolution, true);
    r.globalSetDirty = true;
    if (!ok)
        EngineLog("Shadow map creation failed; shadows are off.");
    return ok;
}

// Mesh pipeline permutations

enum MeshPipelineVariantBit : uint32_t
{
    kVariantBit_Wireframe = 1u << 0,
    kVariantBit_DoubleSided = 1u << 1,
    kVariantBit_Transparent = 1u << 2,
    kVariantBit_DepthReadOnly = 1u << 3,
};

static void FillMeshVertexInput(VkVertexInputBindingDescription& binding, VkVertexInputAttributeDescription attrs[6])
{
    // Matches RenderVertex: pos(vec3)+normal(vec3)+uv(vec2)+color(vec4)+boneIndices(uint16x4)+boneWeights(vec4)
    binding = {};
    binding.binding = 0;
    binding.stride = sizeof(RenderVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderVertex, px) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RenderVertex, nx) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(RenderVertex, u) };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RenderVertex, r) };
    attrs[4] = { 4, 0, VK_FORMAT_R16G16B16A16_UINT, offsetof(RenderVertex, boneIndices) };
    attrs[5] = { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RenderVertex, boneWeights) };
}

// Scene pipelines (variant bits) or, with shadowPipeline, the depth-only shadow pipeline.
static VkPipeline CreateMeshPipeline(Renderer& r, const GPUShaderProgram& program, uint32_t variant, bool shadowPipeline)
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

    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[6];
    FillMeshVertexInput(binding, attrs);
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
    rasterizer.cullMode = (doubleSided || shadowPipeline) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE; // matches D3D11's default (CCW culled as back-facing) under the negative-viewport-height flip
    rasterizer.lineWidth = 1.0f;
    if (shadowPipeline)
    {
        // Same bias as D3D11's shadow rasterizer state (float depth: units of 2^(e-23)).
        rasterizer.depthBiasEnable = VK_TRUE;
        rasterizer.depthBiasConstantFactor = r.shadowFormat == VK_FORMAT_D16_UNORM ? 2.0f : 1000.0f;
        rasterizer.depthBiasSlopeFactor = 2.0f;
    }

    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = shadowPipeline ? VK_SAMPLE_COUNT_1_BIT : r.msaaSamples;

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

    VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = shadowPipeline ? 0u : 1u;
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
    pipelineInfo.renderPass = shadowPipeline ? r.shadowRenderPass : r.sceneRenderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS)
    {
        EngineLogf("vkCreateGraphicsPipelines (mesh variant 0x%X%s) failed", variant, shadowPipeline ? ", shadow" : "");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// Lazily builds (and caches) the pipeline for the requested state combination.
static VkPipeline GetOrCreateMeshPipelineVariant(Renderer& r, GPUShaderProgram& program, bool wireframe, bool doubleSided, bool transparent)
{
    uint32_t variant = 0;
    if (wireframe) variant |= kVariantBit_Wireframe;
    if (doubleSided) variant |= kVariantBit_DoubleSided;
    if (transparent) variant |= kVariantBit_Transparent | kVariantBit_DepthReadOnly;

    if (program.pipelines[variant] == VK_NULL_HANDLE)
    {
        KES_ZONE(RENDER, "Create pipeline variant (cache miss)");
        program.pipelines[variant] = CreateMeshPipeline(r, program, variant, false);
    }
    return program.pipelines[variant];
}

static void DestroyProgramPipelines(Renderer& r, GPUShaderProgram& program)
{
    for (VkPipeline& pipeline : program.pipelines)
    {
        if (pipeline) vkDestroyPipeline(r.device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
}

static bool CreateShaderProgram(Renderer& r, const std::string& vsSource, const std::string& psSource, GPUShaderProgram& outProgram)
{
    outProgram.vertexShader = CreateShaderModuleFromBinary(r, vsSource, ShaderStage::Vertex);
    outProgram.pixelShader = outProgram.vertexShader ? CreateShaderModuleFromBinary(r, psSource, ShaderStage::Pixel) : VK_NULL_HANDLE;
    if (!outProgram.pixelShader)
    {
        DestroyGPUShaderProgram(r, outProgram);
        return false;
    }
    return true;
}

static bool CreateMeshPrograms(Renderer& r)
{
    if (!CreateShaderProgram(r, kEngineShaderMeshDefault, kEngineShaderMeshDefault, r.meshDefaultProgram) ||
        !CreateShaderProgram(r, kEngineShaderShadow, kEngineShaderShadow, r.shadowProgram))
        return false;
    // The common opaque variants up front so the first frame doesn't stall.
    for (bool doubleSided : { false, true })
    {
        if (!GetOrCreateMeshPipelineVariant(r, r.meshDefaultProgram, false, doubleSided, false))
            return false;
    }
    r.shadowProgram.pipelines[0] = CreateMeshPipeline(r, r.shadowProgram, 0, true);
    return r.shadowProgram.pipelines[0] != VK_NULL_HANDLE;
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
    if (!CreateShaderProgram(r, vs, ps, program))
    {
        EngineLogf("Failed to create shader program for \"%s\".", shader->name.c_str());
        return nullptr;
    }

    auto inserted = r.gpuShaders.emplace(handle, program);
    return &inserted.first->second;
}

// Sky / tonemap pipelines (fixed, no vertex input, drawn via gl_VertexIndex)
static VkPipeline CreateFullscreenTrianglePipeline(Renderer& r, VkShaderModule vs, VkShaderModule ps, VkPipelineLayout layout,
    VkRenderPass renderPass, bool hasDepthAttachment, VkSampleCountFlagBits samples)
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
    multisample.rasterizationSamples = samples;

    // The sky tests against the cleared depth without writing it.
    VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = hasDepthAttachment ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
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
    if (!r.skyVS)
        r.skyVS = CreateShaderModuleFromBinary(r, kEngineShaderSky, ShaderStage::Vertex);
    if (!r.skyPS)
        r.skyPS = CreateShaderModuleFromBinary(r, kEngineShaderSky, ShaderStage::Pixel);
    if (!r.skyVS || !r.skyPS)
        return false;
    r.skyPipeline = CreateFullscreenTrianglePipeline(r, r.skyVS, r.skyPS, r.skyPipelineLayout, r.sceneRenderPass, true, r.msaaSamples);
    return r.skyPipeline != VK_NULL_HANDLE;
}

static bool CreateTonemapPipeline(Renderer& r)
{
    r.tonemapVS = CreateShaderModuleFromBinary(r, kEngineShaderBlit, ShaderStage::Vertex);
    r.tonemapPS = CreateShaderModuleFromBinary(r, kEngineShaderBlit, ShaderStage::Pixel);
    if (!r.tonemapVS || !r.tonemapPS)
        return false;
    r.tonemapPipeline = CreateFullscreenTrianglePipeline(r, r.tonemapVS, r.tonemapPS, r.tonemapPipelineLayout, r.backbufferRenderPass,
                                                         false, VK_SAMPLE_COUNT_1_BIT);
    return r.tonemapPipeline != VK_NULL_HANDLE;
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
        if (!CreateBuffer(r, sizeof(render::SceneCBData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, frame.sceneCB, frame.sceneCBMemory))
            return false;
        VK_CHECK_RET(vkMapMemory(r.device, frame.sceneCBMemory, 0, sizeof(render::SceneCBData), 0, &frame.sceneCBMapped), "vkMapMemory (sceneCB)", false);

        if (!CreateBuffer(r, sizeof(render::SkyCBData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, frame.skyCB, frame.skyCBMemory))
            return false;
        VK_CHECK_RET(vkMapMemory(r.device, frame.skyCBMemory, 0, sizeof(render::SkyCBData), 0, &frame.skyCBMapped), "vkMapMemory (skyCB)", false);

        if (!CreateDynamicRingBuffer(r, frame.modelCBRing, sizeof(render::ModelCBData), kInitialDynamicSlotCapacity))
            return false;
        if (!CreateDynamicRingBuffer(r, frame.materialCBRing, sizeof(render::MaterialCBData), kInitialDynamicSlotCapacity))
            return false;

        UpdateSceneAndSkyDescriptorBindings(r, frame);
        UpdateDynamicRingDescriptorBinding(r, frame.uboDescriptorSet, 1, frame.modelCBRing);
        UpdateDynamicRingDescriptorBinding(r, frame.uboDescriptorSet, 2, frame.materialCBRing);
    }
    return true;
}

// Textures

static bool CreateWhiteTexture(Renderer& r)
{
    std::vector<render::MipLevel> levels(1);
    levels[0].width = levels[0].height = 1;
    levels[0].pixels = { 255, 255, 255, 255 };
    return CreateTexture2D(r, levels, VK_FORMAT_R8G8B8A8_UNORM, r.whiteTexture);
}

// Material texture with mips, in the color space its slot needs. Failed loads
// are cached as empty so they aren't retried every frame.
static VkImageView GetMaterialTexture(Renderer& r, const std::string& path, render::TextureUsage usage)
{
    if (path.empty())
        return VK_NULL_HANDLE;
    const std::string key = std::to_string(static_cast<int>(usage)) + "|" + path;
    auto it = r.gpuTextures.find(key);
    if (it != r.gpuTextures.end())
        return it->second.view;

    KES_ZONE(RENDER, "Load material texture (cache miss)");
    KES_ZONE_TEXTF(RENDER, "%s", path.c_str());
    GPUTexture texture = {};
    TextureFileData file = {};
    std::string error;
    if (LoadTextureFile(path.c_str(), file, &error) && file.pixels.size() >= static_cast<size_t>(file.width) * file.height * 4u)
    {
        std::vector<render::MipLevel> levels;
        render::GenerateMips(file.pixels.data(), file.width, file.height, usage, levels);
        const VkFormat format = usage == render::TextureUsage::Color ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        CreateTexture2D(r, levels, format, texture);
    }
    else
    {
        EngineLogf("LoadTextureFile failed for %s: %s", path.c_str(), error.c_str());
    }
    return r.gpuTextures.emplace(key, texture).first->second.view;
}

// Set 1 for a material: its five textures (white where missing). Materials
// with the same textures share a set.
static const GPUMaterialSet& GetMaterialSet(Renderer& r, const MaterialData* material)
{
    static const MaterialData kNone = {};
    const MaterialData& m = material ? *material : kNone;
    const struct { const std::string* path; render::TextureUsage usage; uint32_t flag; } slots[render::kMaterialTextureCount] = {
        { &m.baseTexturePath, render::TextureUsage::Color, MaterialFlag_HasBaseTexture },
        { &m.normalTexturePath, render::TextureUsage::Normal, MaterialFlag_HasNormalTexture },
        { &m.metallicRoughnessTexturePath, render::TextureUsage::Linear, MaterialFlag_HasMetallicRoughnessTexture },
        { &m.occlusionTexturePath, render::TextureUsage::Linear, MaterialFlag_HasOcclusionTexture },
        { &m.emissiveTexturePath, render::TextureUsage::Color, MaterialFlag_HasEmissiveTexture },
    };
    std::string key;
    for (const auto& slot : slots)
        key += *slot.path + '\n';

    auto it = r.gpuMaterialSets.find(key);
    if (it != r.gpuMaterialSets.end())
        return it->second;

    GPUMaterialSet entry;
    VkDescriptorImageInfo images[render::kMaterialTextureCount] = {};
    for (int i = 0; i < render::kMaterialTextureCount; ++i)
    {
        VkImageView view = GetMaterialTexture(r, *slots[i].path, slots[i].usage);
        if (view)
            entry.textureFlags |= slots[i].flag;
        images[i] = { r.materialSampler, view ? view : r.whiteTexture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    entry.set = AllocateDescriptorSet(r, r.materialSetLayout);
    if (entry.set)
    {
        VkWriteDescriptorSet writes[render::kMaterialTextureCount] = {};
        for (uint32_t i = 0; i < render::kMaterialTextureCount; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[i].dstSet = entry.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(r.device, render::kMaterialTextureCount, writes, 0, nullptr);
    }
    return r.gpuMaterialSets.emplace(key, entry).first->second;
}

static void DestroyMaterialSets(Renderer& r)
{
    for (auto& [key, entry] : r.gpuMaterialSets)
    {
        if (entry.set)
            vkFreeDescriptorSets(r.device, r.descriptorPool, 1, &entry.set);
    }
    r.gpuMaterialSets.clear();
}

static bool UploadEnvironment(Renderer& r)
{
    DestroyGPUTexture(r, r.environmentMap);
    const render::EnvironmentMaps& env = r.environment;

    std::vector<uint8_t> bytes;
    std::vector<UploadRegion> regions;
    for (int face = 0; face < 6; ++face)
    {
        for (int mip = 0; mip < env.mipCount; ++mip)
        {
            const uint32_t size = static_cast<uint32_t>(std::max(env.faceSize >> mip, 1));
            const std::vector<uint16_t>& texels = env.faces[mip][face];
            regions.push_back({ static_cast<uint32_t>(mip), static_cast<uint32_t>(face), size, size, bytes.size() });
            const uint8_t* data = reinterpret_cast<const uint8_t*>(texels.data());
            bytes.insert(bytes.end(), data, data + texels.size() * sizeof(uint16_t));
        }
    }

    ImageDesc desc;
    desc.width = desc.height = static_cast<uint32_t>(env.faceSize);
    desc.mipLevels = static_cast<uint32_t>(env.mipCount);
    desc.layers = 6;
    desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    desc.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (!CreateImage(r, desc, r.environmentMap.image, r.environmentMap.memory) ||
        !UploadImage(r, r.environmentMap.image, bytes.data(), bytes.size(), regions, desc.mipLevels, 6))
    {
        DestroyGPUTexture(r, r.environmentMap);
        return false;
    }
    r.environmentMap.view = CreateImageView(r, r.environmentMap.image, desc.format, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 0, 6, desc.mipLevels);
    r.globalSetDirty = true;
    return r.environmentMap.view != VK_NULL_HANDLE;
}

// Mesh upload

static bool UploadMeshBuffers(Renderer& r, const std::vector<RenderVertex>& vertices, const std::vector<uint32_t>& indices, GPUMesh& gpu)
{
    gpu.indexCount = static_cast<uint32_t>(indices.empty() ? vertices.size() : indices.size());
    render::ComputeBoundingSphere(vertices.data(), vertices.size(), sizeof(RenderVertex), gpu.boundsCenter, gpu.boundsRadius);
    if (!CreateDeviceLocalBufferWithData(r, vertices.data(), vertices.size() * sizeof(RenderVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gpu.vertexBuffer, gpu.vertexMemory))
        return false;
    if (!indices.empty() &&
        !CreateDeviceLocalBufferWithData(r, indices.data(), indices.size() * sizeof(uint32_t),
                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT, gpu.indexBuffer, gpu.indexMemory))
    {
        DestroyBuffer(r, gpu.vertexBuffer, gpu.vertexMemory);
        return false;
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
    DestroyProgramPipelines(r, program);
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
    DestroyMaterialSets(r);
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

    // Order matters: immutable samplers before the set layouts, render passes
    // before targets and pipelines, command pools before any upload.
    bool ok = CreateInstance(r, window) && SelectPhysicalDevice(r) && CreateLogicalDevice(r) && ChooseDepthFormats(r);
    if (ok)
        r.msaaSamples = ChooseMsaaSamples(r, r.options.msaaSamples);
    ok = ok && CreateSamplers(r) && CreateDescriptorSetLayouts(r) && CreatePipelineLayouts(r) && CreateDescriptorPool(r) &&
         CreateUboDescriptorSets(r) && CreateCommandObjects(r) && CreateSceneRenderPass(r) && CreateShadowRenderPass(r) &&
         CreateRenderTargets(r) && CreateMeshPrograms(r) && CreateSkyPipeline(r) && CreateTonemapPipeline(r) &&
         CreateUiPipeline(r) && CreateConstantBuffers(r) && CreateWhiteTexture(r) &&
         CreateShadowArray(r, r.dummyShadows, 1, 1, false);
    if (ok)
        CreateShadowTargets(r);   // non-fatal: shadows stay off on failure
    if (ok)
    {
        r.uiWhiteDescriptorSet = AllocateTextureDescriptorSet(r, r.whiteTexture.view, r.uiTexSetLayout);
        ok = r.uiWhiteDescriptorSet != VK_NULL_HANDLE;
    }
    if (!ok)
    {
        RendererShutdown(r);
        return false;
    }

#if KES_PROFILE_GPU
    // Records and submits a few one-off command buffers on the graphics queue
    // to calibrate GPU timestamps; the frame command buffer is reset before use.
    r.gpuProfiler = TracyVkContext(r.physicalDevice, r.device, r.graphicsQueue, r.commandBuffer);
    TracyVkContextName(r.gpuProfiler, "Vulkan", 6);
#endif

    EngineLogf("Renderer initialized successfully (Vulkan). window=%dx%d render=%dx%d, MSAA %dx, shadow format %s.",
               r.windowWidth, r.windowHeight, r.renderWidth, r.renderHeight, static_cast<int>(r.msaaSamples),
               r.shadowFormat == VK_FORMAT_D32_SFLOAT ? "D32" : "D16");
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

void RendererSetGraphicsOptions(Renderer& r, const GraphicsOptions& options)
{
    const GraphicsOptions previous = r.options;
    r.options = options;
    if (!r.device || previous == options || r.frameActive)
        return;

    vkDeviceWaitIdle(r.device);
    const VkSampleCountFlagBits samples = ChooseMsaaSamples(r, options.msaaSamples);
    if (samples != r.msaaSamples)
    {
        // Every scene pipeline is tied to the render pass's sample count.
        DestroySceneTargets(r);
        DestroyProgramPipelines(r, r.meshDefaultProgram);
        for (auto& [handle, program] : r.gpuShaders)
            DestroyProgramPipelines(r, program);
        if (r.skyPipeline) { vkDestroyPipeline(r.device, r.skyPipeline, nullptr); r.skyPipeline = VK_NULL_HANDLE; }
        vkDestroyRenderPass(r.device, r.sceneRenderPass, nullptr);
        r.sceneRenderPass = VK_NULL_HANDLE;
        r.msaaSamples = samples;
        if (!CreateSceneRenderPass(r) || !CreateSceneTargets(r) || !CreateSkyPipeline(r))
            EngineLog("Recreating the scene pass for the new MSAA setting failed.");
    }
    if (previous.shadowQuality != options.shadowQuality)
        CreateShadowTargets(r);
    if (previous.anisotropy != options.anisotropy)
    {
        DestroyMaterialSets(r);
        CreateMaterialSampler(r);
    }
    EngineLogf("Graphics options: MSAA %dx, shadows %d, anisotropy %dx.", static_cast<int>(r.msaaSamples),
               static_cast<int>(options.shadowQuality), std::clamp(options.anisotropy, 1, 16));
}

// Per-draw dynamic constant writes
// Writes only what the shader reads: the two matrices, the skin info and the
// skin matrices that are actually used. ModelCBData is ~8 KB because of the
// 128-matrix skin palette, too much to copy for every static mesh draw.
static VkDeviceSize WriteModelConstants(Renderer& r, const Matrix4& wvp, const Matrix4& world, const float* skinMatrices, uint32_t skinMatrixCount)
{
    const VkDeviceSize offset = AcquireDynamicSlot(r, r.frames[r.frameIndex].modelCBRing, 1);
    uint8_t* dst = r.frames[r.frameIndex].modelCBRing.mapped + offset;
    std::memcpy(dst + offsetof(render::ModelCBData, worldViewProj), wvp.m, sizeof(wvp.m));
    std::memcpy(dst + offsetof(render::ModelCBData, world), world.m, sizeof(world.m));
    if (skinMatrices && skinMatrixCount > 0)
        std::memcpy(dst + offsetof(render::ModelCBData, skinMatrices), skinMatrices, skinMatrixCount * 16u * sizeof(float));
    const float skinInfo[4] = { skinMatrixCount > 0 ? 1.0f : 0.0f, static_cast<float>(skinMatrixCount), 0.0f, 0.0f };
    std::memcpy(dst + offsetof(render::ModelCBData, skinInfo), skinInfo, sizeof(skinInfo));
    return offset;
}

static VkDeviceSize WriteMaterialConstants(Renderer& r, const MaterialData* material, uint32_t textureFlags)
{
    const render::MaterialCBData cb = render::BuildMaterialConstants(material, textureFlags);
    const VkDeviceSize offset = AcquireDynamicSlot(r, r.frames[r.frameIndex].materialCBRing, 2);
    std::memcpy(r.frames[r.frameIndex].materialCBRing.mapped + offset, &cb, sizeof(cb));
    return offset;
}

// Viewports (negative-height trick: makes Vulkan's screen-space Y and
// depth-range conventions match D3D11's, so the shared camera/projection
// math, shadow matrices and every clip-space computation need no changes)

static VkViewport BuildFlippedViewport(float width, float height)
{
    VkViewport vp = {};
    vp.y = height;
    vp.width = width;
    vp.height = -height;
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

static void RenderSkyBackground(Renderer& r, const Camera& camera, const Scene& sceneObj)
{
    if (sceneObj.GetFog().active || !sceneObj.GetSky().active)
        return;

    const float aspect = static_cast<float>(r.renderWidth) / static_cast<float>(r.renderHeight);
    const render::SkyCBData skyCB = render::BuildSkyConstants(sceneObj.GetSky(), camera, aspect);
    std::memcpy(r.frames[r.frameIndex].skyCBMapped, &skyCB, sizeof(skyCB));

    vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.skyPipeline);
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.skyPipelineLayout, 0, 1, &r.skyDescriptorSet, 0, nullptr);
    vkCmdDraw(r.commandBuffer, 3, 1, 0, 0);
}

// Frame lifecycle

// Starts recording this frame's command buffer the first time any render
// call is made (D3D11's immediate context has no explicit "begin frame").
static void BeginFrameIfNeeded(Renderer& r)
{
    if (r.frameActive || !r.fatalError.empty())
        return;

    KES_ZONE_FN(RENDER);
    FrameResources& frame = r.frames[r.frameIndex];
    {
        // Time blocked here = CPU waiting on the GPU (or on vsync).
        KES_ZONE(RENDER, "Wait for GPU (in-flight fence)");
        if (!CheckGpuResult(r, vkWaitForFences(r.device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
            return;

        // Under vsync, running a frame ahead only makes the CPU wait in bursts
        // (alternating ~0 ms and two refresh intervals), and the uneven frame
        // deltas show up as judder. Wait for the previous frame too, so vsync
        // gets one frame in flight; without vsync frames overlap for throughput.
        if (r.vsync)
        {
            const FrameResources& previous = r.frames[(r.frameIndex + kMaxFramesInFlight - 1) % kMaxFramesInFlight];
            if (!CheckGpuResult(r, vkWaitForFences(r.device, 1, &previous.inFlightFence, VK_TRUE, UINT64_MAX), "vkWaitForFences"))
                return;
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

// Redundant-state filter for consecutive draws.
struct DrawState
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    const MaterialData* material = nullptr;
    bool haveMaterial = false;
    VkDeviceSize materialOffset = 0;
    VkDescriptorSet materialSet = VK_NULL_HANDLE;
};

static void DrawGPUMesh(Renderer& r, DrawState& state, const GPUMesh& gpuMesh, VkDeviceSize modelOffset)
{
    const uint32_t dynamicOffsets[2] = { static_cast<uint32_t>(modelOffset), static_cast<uint32_t>(state.materialOffset) };
    VkDescriptorSet sets[2] = { r.uboDescriptorSet, state.materialSet };
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.meshPipelineLayout, 0, 2, sets, 2, dynamicOffsets);

    if (gpuMesh.vertexBuffer != state.vertexBuffer)
    {
        const VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(r.commandBuffer, 0, 1, &gpuMesh.vertexBuffer, &vertexOffset);
        state.vertexBuffer = gpuMesh.vertexBuffer;
    }
    if (gpuMesh.indexBuffer)
    {
        if (gpuMesh.indexBuffer != state.indexBuffer)
        {
            vkCmdBindIndexBuffer(r.commandBuffer, gpuMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            state.indexBuffer = gpuMesh.indexBuffer;
        }
        vkCmdDrawIndexed(r.commandBuffer, gpuMesh.indexCount, 1, 0, 0, 0);
    }
    else
    {
        vkCmdDraw(r.commandBuffer, gpuMesh.indexCount, 1, 0, 0);
    }
}

// Draws one scene object with `viewProj` (camera or shadow view), culled against `frustum`.
static void DrawSceneObject(Renderer& r, DrawState& state, const DrawItem& item, const Matrix4& viewProj, const render::Frustum& frustum,
                            const AssetSystem& assets, bool shadowPass, bool wireframe, bool transparent)
{
    const SceneObject& obj = *item.object;
    const KModelData* model = assets.GetModel(obj.model);
    const bool skinned = model && !model->bones.empty() && obj.skinMatrixCount > 0;

    VkPipeline pipeline = r.shadowProgram.pipelines[0];
    if (!shadowPass)
    {
        GPUShaderProgram* program = &r.meshDefaultProgram;
        if (item.material && item.material->shader != 0)
        {
            if (GPUShaderProgram* custom = GetOrCompileShaderProgram(r, item.material->shader, assets))
                program = custom;
        }
        pipeline = GetOrCreateMeshPipelineVariant(r, *program, wireframe, IsDoubleSidedMaterial(item.material), transparent);
    }
    if (!pipeline)
        return;
    if (pipeline != state.pipeline)
    {
        vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        state.pipeline = pipeline;
    }

    // Material constants and textures are identical for every draw of a material.
    if (!state.haveMaterial || item.material != state.material)
    {
        const GPUMaterialSet& materialSet = GetMaterialSet(r, item.material);
        state.materialSet = materialSet.set;
        state.materialOffset = WriteMaterialConstants(r, item.material, materialSet.textureFlags);
        state.material = item.material;
        state.haveMaterial = true;
    }
    if (!state.materialSet)
        return;

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
            const VkDeviceSize modelOffset = WriteModelConstants(r, render::MatrixMultiply(world, viewProj), world, obj.skinMatrices, matrixCount);
            DrawGPUMesh(r, state, *gpuMesh, modelOffset);
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
    const VkDeviceSize modelOffset = WriteModelConstants(r, render::MatrixMultiply(objectWorld, viewProj), objectWorld, nullptr, 0);
    DrawGPUMesh(r, state, *gpuMesh, modelOffset);
}

static void RenderShadowMaps(Renderer& r, const AssetSystem& assets)
{
    if (r.frame.shadowViews.empty())
        return;
    KES_ZONE(RENDER, "Shadow maps");
    KES_VK_GPU_ZONE(r, "Shadows");

    for (const render::ShadowView& view : r.frame.shadowViews)
    {
        GPUShadowArray& target = view.cascade ? r.cascadeShadows : r.localShadows;
        if (view.slice >= static_cast<int>(target.framebuffers.size()))
            continue;

        VkClearValue clear = {};
        clear.depthStencil = { 1.0f, 0 };
        VkRenderPassBeginInfo passBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        passBegin.renderPass = r.shadowRenderPass;
        passBegin.framebuffer = target.framebuffers[static_cast<size_t>(view.slice)];
        passBegin.renderArea.extent = { static_cast<uint32_t>(target.resolution), static_cast<uint32_t>(target.resolution) };
        passBegin.clearValueCount = 1;
        passBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(r.commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

        const float size = static_cast<float>(target.resolution);
        const VkViewport viewport = BuildFlippedViewport(size, size);
        vkCmdSetViewport(r.commandBuffer, 0, 1, &viewport);
        const VkRect2D scissor = { { 0, 0 }, passBegin.renderArea.extent };
        vkCmdSetScissor(r.commandBuffer, 0, 1, &scissor);

        // The light's frustum (far plane = range for local lights) culls casters.
        const render::Frustum frustum = render::ExtractFrustum(view.viewProj);
        DrawState state;
        for (const DrawItem& item : r.opaqueDrawItems)
        {
            if (CastsShadows(item.material))
                DrawSceneObject(r, state, item, view.viewProj, frustum, assets, true, false, false);
        }
        vkCmdEndRenderPass(r.commandBuffer);
    }
}

void RendererRenderScene(Renderer& r, const SceneState& scene, const Camera& camera, const Scene& sceneObj, const AssetSystem& assets)
{
    if (!r.fatalError.empty())
        return;

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
    if (!r.sceneFramebuffer || !r.swapchain)
        return;   // targets unavailable (window minimized); retry next frame

    // Ambient environment, rebuilt when the sky (or flat ambient) changes.
    const uint64_t environmentKey = render::EnvironmentKey(scene, sceneObj);
    if (!r.frameActive && (environmentKey != r.environment.key || !r.environmentMap.view))
    {
        KES_ZONE(RENDER, "Build environment");
        vkDeviceWaitIdle(r.device);   // the old cube may still be sampled
        render::BuildEnvironment(scene, sceneObj, r.environment);
        UploadEnvironment(r);
    }
    if (!r.environmentMap.view)
        return;
    if (r.globalSetDirty && !r.frameActive)
    {
        vkDeviceWaitIdle(r.device);
        UpdateGlobalDescriptorSet(r);
    }

    const float aspect = static_cast<float>(r.renderWidth) / static_cast<float>(r.renderHeight);
    render::BuildFrameSetup(r.frame, scene, camera, sceneObj, aspect, r.options);
    render::ApplyEnvironment(r.environment, r.frame.scene);
    if (!r.cascadeShadows.arrayView)
    {
        r.frame.scene.shadowParams[3] = 0.0f;
        r.frame.shadowViews.clear();
    }
    r.exposure = std::max(scene.exposure, 0.0f);

    BeginFrameIfNeeded(r);
    if (!r.frameActive)
        return;
    std::memcpy(r.frames[r.frameIndex].sceneCBMapped, &r.frame.scene, sizeof(r.frame.scene));

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

    KES_VK_GPU_ZONE(r, "Scene");
    float clearColor[4] = {};
    BuildClearColor(sceneObj, clearColor);
    VkClearValue clearValues[3] = {};
    clearValues[0].color = { { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo passBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    passBegin.renderPass = r.sceneRenderPass;
    passBegin.framebuffer = r.sceneFramebuffer;
    passBegin.renderArea.extent = { static_cast<uint32_t>(r.renderWidth), static_cast<uint32_t>(r.renderHeight) };
    passBegin.clearValueCount = r.msaaSamples != VK_SAMPLE_COUNT_1_BIT ? 3u : 2u;
    passBegin.pClearValues = clearValues;
    vkCmdBeginRenderPass(r.commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport = BuildFlippedViewport(static_cast<float>(r.renderWidth), static_cast<float>(r.renderHeight));
    vkCmdSetViewport(r.commandBuffer, 0, 1, &viewport);
    const VkRect2D scissor = { { 0, 0 }, passBegin.renderArea.extent };
    vkCmdSetScissor(r.commandBuffer, 0, 1, &scissor);

    RenderSkyBackground(r, camera, sceneObj);

    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.meshPipelineLayout, 2, 1, &r.globalDescriptorSet, 0, nullptr);
    DrawState state;
    {
        KES_ZONE(RENDER, "Draw opaque");
        KES_VK_GPU_ZONE(r, "Opaque");
        for (const DrawItem& item : opaqueItems)
            DrawSceneObject(r, state, item, r.frame.viewProj, r.frame.frustum, assets, false, scene.wireframe, false);
    }
    {
        KES_ZONE(RENDER, "Draw transparent");
        KES_VK_GPU_ZONE(r, "Transparent");
        for (const DrawItem& item : transparentItems)
            DrawSceneObject(r, state, item, r.frame.viewProj, r.frame.frustum, assets, false, scene.wireframe, true);
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
    if (!CheckGpuResult(r, acquireResult, "vkAcquireNextImageKHR"))
    {
        r.haveSwapchainImage = false;
        return;
    }

    r.haveSwapchainImage = true;
    KES_VK_GPU_ZONE(r, "Tonemap");

    VkClearValue clearValue = {};
    clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderPassBeginInfo passBegin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    passBegin.renderPass = r.backbufferRenderPass;
    passBegin.framebuffer = r.backbufferFramebuffers[r.currentSwapchainImageIndex];
    passBegin.renderArea.extent = r.swapchainExtent;
    passBegin.clearValueCount = 1;
    passBegin.pClearValues = &clearValue;
    vkCmdBeginRenderPass(r.commandBuffer, &passBegin, VK_SUBPASS_CONTENTS_INLINE);

    const VkViewport viewport = BuildPresentationViewport(r);
    vkCmdSetViewport(r.commandBuffer, 0, 1, &viewport);
    const VkRect2D scissor = { { 0, 0 }, r.swapchainExtent };
    vkCmdSetScissor(r.commandBuffer, 0, 1, &scissor);

    render::TonemapCBData tonemap = {};
    tonemap.params[0] = r.exposure;
    tonemap.params[1] = 1.0f / 255.0f;
    vkCmdBindPipeline(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.tonemapPipeline);
    vkCmdBindDescriptorSets(r.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, r.tonemapPipelineLayout, 0, 1, &r.tonemapDescriptorSet, 0, nullptr);
    vkCmdPushConstants(r.commandBuffer, r.tonemapPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tonemap), &tonemap);
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

    std::vector<render::MipLevel> levels(1);
    levels[0].width = static_cast<uint32_t>(width);
    levels[0].height = static_cast<uint32_t>(height);
    levels[0].pixels.assign(rgbaPixels, rgbaPixels + static_cast<size_t>(width) * height * 4u);
    GPUTexture texture = {};
    if (CreateTexture2D(r, levels, VK_FORMAT_R8G8B8A8_UNORM, texture))
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
        if (!CheckGpuResult(r, vkQueueSubmit(r.graphicsQueue, 1, &submitInfo, r.frames[r.frameIndex].inFlightFence), "vkQueueSubmit"))
            r.haveSwapchainImage = false;
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
        else
            CheckGpuResult(r, presentResult, "vkQueuePresentKHR");
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
    if (!r.device)
    {
        if (r.surface) { SDL_Vulkan_DestroySurface(r.instance, r.surface, nullptr); r.surface = VK_NULL_HANDLE; }
        if (r.instance) { vkDestroyInstance(r.instance, nullptr); r.instance = VK_NULL_HANDLE; }
        return;
    }
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
    DestroyGPUTexture(r, r.environmentMap);
    r.environment.key = 0;
    DestroyShadowArray(r, r.cascadeShadows);
    DestroyShadowArray(r, r.localShadows);
    DestroyShadowArray(r, r.dummyShadows);
    DestroyRendererAssetCaches(r);
    r.assetGeneration = 0;

    for (FrameResources& frame : r.frames)
    {
        DestroyStreamBuffer(r, frame.uiVertices);
        DestroyStreamBuffer(r, frame.uiIndices);
        if (frame.inFlightFence) { vkDestroyFence(r.device, frame.inFlightFence, nullptr); frame.inFlightFence = VK_NULL_HANDLE; }
        if (frame.imageAvailableSemaphore) { vkDestroySemaphore(r.device, frame.imageAvailableSemaphore, nullptr); frame.imageAvailableSemaphore = VK_NULL_HANDLE; }
        DestroyDynamicRingBuffer(r, frame.modelCBRing);
        DestroyDynamicRingBuffer(r, frame.materialCBRing);
        if (frame.sceneCBMapped) { vkUnmapMemory(r.device, frame.sceneCBMemory); frame.sceneCBMapped = nullptr; }
        DestroyBuffer(r, frame.sceneCB, frame.sceneCBMemory);
        if (frame.skyCBMapped) { vkUnmapMemory(r.device, frame.skyCBMemory); frame.skyCBMapped = nullptr; }
        DestroyBuffer(r, frame.skyCB, frame.skyCBMemory);
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

    const struct { VkPipeline* pipeline; VkShaderModule* vs; VkShaderModule* ps; } fixedPipelines[] = {
        { &r.uiPipeline, &r.uiVS, &r.uiPS }, { &r.tonemapPipeline, &r.tonemapVS, &r.tonemapPS }, { &r.skyPipeline, &r.skyVS, &r.skyPS } };
    for (const auto& entry : fixedPipelines)
    {
        if (*entry.pipeline) { vkDestroyPipeline(r.device, *entry.pipeline, nullptr); *entry.pipeline = VK_NULL_HANDLE; }
        if (*entry.vs) { vkDestroyShaderModule(r.device, *entry.vs, nullptr); *entry.vs = VK_NULL_HANDLE; }
        if (*entry.ps) { vkDestroyShaderModule(r.device, *entry.ps, nullptr); *entry.ps = VK_NULL_HANDLE; }
    }
    DestroyGPUShaderProgram(r, r.meshDefaultProgram);
    DestroyGPUShaderProgram(r, r.shadowProgram);

    DestroyRenderTargets(r);
    for (VkRenderPass* pass : { &r.sceneRenderPass, &r.shadowRenderPass, &r.backbufferRenderPass })
    {
        if (*pass) { vkDestroyRenderPass(r.device, *pass, nullptr); *pass = VK_NULL_HANDLE; }
    }

    // Descriptor sets allocated from a FREE_DESCRIPTOR_SET_BIT pool are
    // released with the pool itself.
    if (r.descriptorPool) { vkDestroyDescriptorPool(r.device, r.descriptorPool, nullptr); r.descriptorPool = VK_NULL_HANDLE; }
    for (VkPipelineLayout* layout : { &r.meshPipelineLayout, &r.skyPipelineLayout, &r.tonemapPipelineLayout, &r.uiPipelineLayout })
    {
        if (*layout) { vkDestroyPipelineLayout(r.device, *layout, nullptr); *layout = VK_NULL_HANDLE; }
    }
    for (VkDescriptorSetLayout* layout : { &r.uiTexSetLayout, &r.uboSetLayout, &r.materialSetLayout, &r.globalSetLayout, &r.texSetLayout, &r.skySetLayout })
    {
        if (*layout) { vkDestroyDescriptorSetLayout(r.device, *layout, nullptr); *layout = VK_NULL_HANDLE; }
    }
    for (VkSampler* sampler : { &r.materialSampler, &r.linearSampler, &r.shadowSampler })
    {
        if (*sampler) { vkDestroySampler(r.device, *sampler, nullptr); *sampler = VK_NULL_HANDLE; }
    }

    vkDestroyDevice(r.device, nullptr);
    r.device = VK_NULL_HANDLE;

    if (r.surface) { SDL_Vulkan_DestroySurface(r.instance, r.surface, nullptr); r.surface = VK_NULL_HANDLE; }

#ifndef NDEBUG
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
        void SetGraphicsOptions(const GraphicsOptions& options) override { kes::vk::RendererSetGraphicsOptions(m_state, options); }
        GraphicsOptions GetGraphicsOptions() const override { return m_state.options; }

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
        const std::string& GetFatalError() const override { return m_state.fatalError; }

    private:
        kes::vk::Renderer m_state = {};
        bool m_initialized = false;
    };
}

std::unique_ptr<IRenderer> CreateVulkanRenderer()
{
    return std::make_unique<VulkanRenderer>();
}

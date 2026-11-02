#include "render_backend.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include "command_line.h"
#include "engine_configuration.h"
#include "logger.h"

// Build configuration (set by engine/CMakeLists.txt):
//   KES_HAS_RENDERER_VULKAN / KES_HAS_RENDERER_D3D11  - backends compiled in
//   KES_RENDERER_PRIORITY        - platform priority list, e.g. "vulkan;d3d11"
//   KES_RENDERER_ALLOW_SELECTION - 0 forces the first backend in the list
#ifndef KES_HAS_RENDERER_VULKAN
#define KES_HAS_RENDERER_VULKAN 0
#endif
#ifndef KES_HAS_RENDERER_D3D11
#define KES_HAS_RENDERER_D3D11 0
#endif
#ifndef KES_RENDERER_PRIORITY
#define KES_RENDERER_PRIORITY "vulkan;d3d11"
#endif
#ifndef KES_RENDERER_ALLOW_SELECTION
#define KES_RENDERER_ALLOW_SELECTION 1
#endif

#if !KES_HAS_RENDERER_VULKAN && !KES_HAS_RENDERER_D3D11
#error "No renderer backend enabled for this platform. Check KES_RENDERERS_<PLATFORM> in engine/CMakeLists.txt."
#endif

#if KES_HAS_RENDERER_VULKAN
std::unique_ptr<IRenderer> CreateVulkanRenderer();
#endif
#if KES_HAS_RENDERER_D3D11
std::unique_ptr<IRenderer> CreateD3D11Renderer();
#endif

namespace
{

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    bool IsCompiledIn(RendererBackend backend)
    {
        switch (backend)
        {
        case RendererBackend::Vulkan: return KES_HAS_RENDERER_VULKAN != 0;
        case RendererBackend::D3D11:  return KES_HAS_RENDERER_D3D11 != 0;
        default:                      return false;
        }
    }

    std::vector<RendererBackend> BuildAvailableList()
    {
        std::vector<RendererBackend> list;
        const std::string_view priority = KES_RENDERER_PRIORITY;
        size_t start = 0;
        while (start <= priority.size())
        {
            size_t end = priority.find_first_of(";,", start);
            if (end == std::string_view::npos)
                end = priority.size();

            RendererBackend backend = RendererBackend::Auto;
            const std::string token(priority.substr(start, end - start));
            if (ParseRendererBackend(token.c_str(), backend) &&
                backend != RendererBackend::Auto &&
                IsCompiledIn(backend) &&
                std::find(list.begin(), list.end(), backend) == list.end())
            {
                list.push_back(backend);
            }
            start = end + 1;
        }

        // Anything compiled in but missing from the priority list goes last.
        for (RendererBackend backend : { RendererBackend::Vulkan, RendererBackend::D3D11 })
        {
            if (IsCompiledIn(backend) && std::find(list.begin(), list.end(), backend) == list.end())
                list.push_back(backend);
        }
        return list;
    }

    RendererBackend ReadCommandLineRequest()
    {
        std::string value;
        RendererBackend backend = RendererBackend::Auto;
        if (CommandLineGetValue("renderer", value) && !ParseRendererBackend(value.c_str(), backend))
            EngineLogf("Ignoring unknown renderer \"%s\" on the command line.", value.c_str());
        return backend;
    }

    RendererBackend ReadEnvironmentRequest()
    {
        const char* value = std::getenv("KES_RENDERER");
        RendererBackend backend = RendererBackend::Auto;
        if (value && value[0] && !ParseRendererBackend(value, backend))
            EngineLogf("Ignoring unknown KES_RENDERER value \"%s\".", value);
        return backend;
    }
}

const char* RendererBackendName(RendererBackend backend)
{
    switch (backend)
    {
    case RendererBackend::Vulkan: return "vulkan";
    case RendererBackend::D3D11:  return "d3d11";
    default:                      return "auto";
    }
}

bool ParseRendererBackend(const char* text, RendererBackend& outBackend)
{
    if (!text)
        return false;

    const std::string_view value(text);
    if (EqualsIgnoreCase(value, "auto") || EqualsIgnoreCase(value, "default"))
        outBackend = RendererBackend::Auto;
    else if (EqualsIgnoreCase(value, "vulkan") || EqualsIgnoreCase(value, "vk"))
        outBackend = RendererBackend::Vulkan;
    else if (EqualsIgnoreCase(value, "d3d11") || EqualsIgnoreCase(value, "dx11") || EqualsIgnoreCase(value, "directx11"))
        outBackend = RendererBackend::D3D11;
    else
        return false;
    return true;
}

const std::vector<RendererBackend>& GetAvailableRendererBackends()
{
    static const std::vector<RendererBackend> list = BuildAvailableList();
    return list;
}

bool IsRendererBackendAvailable(RendererBackend backend)
{
    const auto& list = GetAvailableRendererBackends();
    return std::find(list.begin(), list.end(), backend) != list.end();
}

bool IsRendererSelectionAllowed()
{
    return KES_RENDERER_ALLOW_SELECTION != 0 && GetAvailableRendererBackends().size() > 1;
}

SDL_WindowFlags GetRendererWindowFlags(RendererBackend backend)
{
    switch (backend)
    {
    case RendererBackend::Vulkan: return SDL_WINDOW_VULKAN;
    default:                      return 0;
    }
}

std::vector<RendererBackend> BuildRendererCandidateList(RendererBackend configured)
{
    const std::vector<RendererBackend>& available = GetAvailableRendererBackends();
    std::vector<RendererBackend> candidates;

    RendererBackend requested = RendererBackend::Auto;
    const char* source = "platform default";
    if (IsRendererSelectionAllowed())
    {
        // Priority: command line > KES_RENDERER environment variable > AppConfig.
        const RendererBackend fromCommandLine = ReadCommandLineRequest();
        const RendererBackend fromEnv = ReadEnvironmentRequest();
        if (fromCommandLine != RendererBackend::Auto)
        {
            requested = fromCommandLine;
            source = "command line";
        }
        else if (fromEnv != RendererBackend::Auto)
        {
            requested = fromEnv;
            source = "KES_RENDERER";
        }
        else if (configured != RendererBackend::Auto)
        {
            requested = configured;
            source = "AppConfig";
        }

        if (requested != RendererBackend::Auto && !IsRendererBackendAvailable(requested))
        {
            EngineLogf("Renderer \"%s\" requested by %s is not available in this build; using platform default.",
                       RendererBackendName(requested), source);
            requested = RendererBackend::Auto;
            source = "platform default";
        }
    }
    else if (configured != RendererBackend::Auto || ReadCommandLineRequest() != RendererBackend::Auto)
    {
        EngineLogf("Renderer selection is disabled for this platform; forcing \"%s\".",
                   available.empty() ? "none" : RendererBackendName(available.front()));
    }

    if (requested != RendererBackend::Auto)
        candidates.push_back(requested);
    for (RendererBackend backend : available)
    {
        if (std::find(candidates.begin(), candidates.end(), backend) == candidates.end())
            candidates.push_back(backend);
    }

    if (!candidates.empty())
        EngineLogf("Renderer selection: \"%s\" (%s).", RendererBackendName(candidates.front()), source);
    return candidates;
}

std::unique_ptr<IRenderer> CreateRenderer(RendererBackend backend)
{
    switch (backend)
    {
#if KES_HAS_RENDERER_VULKAN
    case RendererBackend::Vulkan: return CreateVulkanRenderer();
#endif
#if KES_HAS_RENDERER_D3D11
    case RendererBackend::D3D11:  return CreateD3D11Renderer();
#endif
    default:                      return nullptr;
    }
}

void RendererUpdateScene(SceneState& scene, const InputState& input)
{
#if ENGINE_ALLOW_DEBUG_MODE
    if (InputKeyPressed(input, SDL_SCANCODE_F1))
        scene.wireframe = !scene.wireframe;

    if (InputKeyPressed(input, SDL_SCANCODE_F3))
        scene.animDebug = !scene.animDebug;
#else
    (void)scene;
    (void)input;
#endif
}

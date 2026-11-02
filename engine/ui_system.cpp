#include "ui_system.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>
#include <RmlUi_Platform_SDL.h>

#include "engine_configuration.h"
#include "logger.h"
#include "pak_file_system.h"
#include "profiler.h"
#include "texture_file.h"

namespace
{
    // Fonts loaded at startup (engine/assets/engine/ui/fonts): font family
    // "LatoLatin", the default in engine/ui/kes.rcss. The regular face is also
    // the fallback for glyphs missing from other fonts.
    constexpr const char *kEngineFonts[] = {
        "engine/ui/fonts/LatoLatin-Regular.ttf",
        "engine/ui/fonts/LatoLatin-Bold.ttf",
        "engine/ui/fonts/LatoLatin-Italic.ttf",
        "engine/ui/fonts/LatoLatin-BoldItalic.ttf",
    };

    std::string Trim(const std::string &text)
    {
        const size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return {};
        const size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    // System interface: SDL time, cursors, clipboard and IME from RmlUi's SDL
    // platform code; log messages go to the engine log.
    class KesUiSystemInterface final : public SystemInterface_SDL
    {
    public:
        explicit KesUiSystemInterface(SDL_Window *window) : SystemInterface_SDL(window) {}

        bool LogMessage(Rml::Log::Type type, const Rml::String &message) override
        {
            const char *level = "Info";
            switch (type)
            {
            case Rml::Log::LT_ERROR:
            case Rml::Log::LT_ASSERT:
                level = "Error";
                break;
            case Rml::Log::LT_WARNING:
                level = "Warning";
                break;
            case Rml::Log::LT_DEBUG:
                level = "Debug";
                break;
            default:
                break;
            }
            EngineLogf("[UI] %s: %s", level, message.c_str());
            return true;   // never break into the debugger from UI logs
        }
    };

    // File interface: loose content or mounted paks (LoadAssetFileBytes).
    // Files are small (documents, stylesheets, fonts), so they are read whole.
    class KesUiFileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(const Rml::String &path) override
        {
            KES_ZONE(UI, "UI file open");
            KES_ZONE_TEXT(UI, path.c_str(), path.size());
            auto file = std::make_unique<OpenFile>();
            std::string error;
            if (!LoadAssetFileBytes(path.c_str(), file->data, &error))
                return 0;
            return reinterpret_cast<Rml::FileHandle>(file.release());
        }

        void Close(Rml::FileHandle handle) override { delete Get(handle); }

        size_t Read(void *buffer, size_t size, Rml::FileHandle handle) override
        {
            OpenFile *file = Get(handle);
            const size_t count = std::min(size, file->data.size() - file->position);
            if (count > 0)
                std::memcpy(buffer, file->data.data() + file->position, count);
            file->position += count;
            return count;
        }

        bool Seek(Rml::FileHandle handle, long offset, int origin) override
        {
            OpenFile *file = Get(handle);
            long long base = 0;
            if (origin == SEEK_CUR)
                base = static_cast<long long>(file->position);
            else if (origin == SEEK_END)
                base = static_cast<long long>(file->data.size());
            const long long target = base + offset;
            if (target < 0 || target > static_cast<long long>(file->data.size()))
                return false;
            file->position = static_cast<size_t>(target);
            return true;
        }

        size_t Tell(Rml::FileHandle handle) override { return Get(handle)->position; }
        size_t Length(Rml::FileHandle handle) override { return Get(handle)->data.size(); }

        bool LoadFile(const Rml::String &path, Rml::String &outData) override
        {
            KES_ZONE(UI, "UI file load");
            KES_ZONE_TEXT(UI, path.c_str(), path.size());
            std::vector<uint8_t> bytes;
            std::string error;
            if (!LoadAssetFileBytes(path.c_str(), bytes, &error))
            {
                Rml::Log::Message(Rml::Log::LT_ERROR, "Could not load '%s': %s", path.c_str(), error.c_str());
                return false;
            }
            outData.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            return true;
        }

    private:
        struct OpenFile
        {
            std::vector<uint8_t> data;
            size_t position = 0;
        };
        static OpenFile *Get(Rml::FileHandle handle) { return reinterpret_cast<OpenFile *>(handle); }
    };

    // Render interface: turns RmlUi geometry into a UiDrawList that the active
    // renderer backend draws (IRenderer::RenderUi). Translation and transforms
    // are applied on the CPU, so the backends only need one simple pipeline.
    // Not implemented (RmlUi skips them): clip masks for transformed overflow,
    // layers and shaders, i.e. box-shadow, filters, mask-image and the
    // linear/radial/conic-gradient decorators. horizontal-/vertical-gradient
    // decorators are plain geometry and work.
    class KesUiRenderInterface final : public Rml::RenderInterface
    {
    public:
        IRenderer *renderer = nullptr;
        UiDrawList drawList;

        void BeginFrame()
        {
            drawList.Clear();
            m_scissorEnabled = false;
            m_hasTransform = false;
        }

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override
        {
            auto *geometry = new CompiledGeometry();
            geometry->vertices.assign(vertices.begin(), vertices.end());
            geometry->indices.assign(indices.begin(), indices.end());
            return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override
        {
            delete reinterpret_cast<CompiledGeometry *>(handle);
        }

        void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override
        {
            const auto *geometry = reinterpret_cast<const CompiledGeometry *>(handle);
            if (!geometry || geometry->indices.empty())
                return;

            const uint32_t baseVertex = static_cast<uint32_t>(drawList.vertices.size());
            const uint32_t firstIndex = static_cast<uint32_t>(drawList.indices.size());

            drawList.vertices.reserve(drawList.vertices.size() + geometry->vertices.size());
            for (const Rml::Vertex &source : geometry->vertices)
            {
                UiVertex vertex;
                Rml::Vector2f position = source.position + translation;
                if (m_hasTransform)
                {
                    const Rml::Vector4f projected = m_transform * Rml::Vector4f(position.x, position.y, 0.0f, 1.0f);
                    const float w = projected.w != 0.0f ? projected.w : 1.0f;
                    position = {projected.x / w, projected.y / w};
                }
                vertex.x = position.x;
                vertex.y = position.y;
                vertex.color[0] = source.colour.red;
                vertex.color[1] = source.colour.green;
                vertex.color[2] = source.colour.blue;
                vertex.color[3] = source.colour.alpha;
                vertex.u = source.tex_coord.x;
                vertex.v = source.tex_coord.y;
                drawList.vertices.push_back(vertex);
            }

            drawList.indices.reserve(drawList.indices.size() + geometry->indices.size());
            for (const int index : geometry->indices)
                drawList.indices.push_back(baseVertex + static_cast<uint32_t>(index));

            const uint32_t indexCount = static_cast<uint32_t>(geometry->indices.size());
            const UiTextureHandle uiTexture = static_cast<UiTextureHandle>(texture);

            // Merge with the previous draw when nothing but the geometry changed.
            if (!drawList.commands.empty())
            {
                UiDrawCommand &last = drawList.commands.back();
                const bool sameScissor = last.scissorEnabled == m_scissorEnabled &&
                                         (!m_scissorEnabled ||
                                          (last.scissor.x == m_scissor.x && last.scissor.y == m_scissor.y &&
                                           last.scissor.width == m_scissor.width && last.scissor.height == m_scissor.height));
                if (last.texture == uiTexture && sameScissor && last.firstIndex + last.indexCount == firstIndex)
                {
                    last.indexCount += indexCount;
                    return;
                }
            }

            UiDrawCommand command;
            command.texture = uiTexture;
            command.firstIndex = firstIndex;
            command.indexCount = indexCount;
            command.scissorEnabled = m_scissorEnabled;
            command.scissor = m_scissor;
            drawList.commands.push_back(command);
        }

        Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override
        {
            KES_ZONE(UI, "UI load texture");
            KES_ZONE_TEXT(UI, source.c_str(), source.size());
            if (!renderer)
                return 0;

            TextureFileData texture;
            std::string error;
            if (!LoadTextureFile(source.c_str(), texture, &error) || texture.pixels.size() < size_t(texture.width) * texture.height * 4u)
            {
                Rml::Log::Message(Rml::Log::LT_WARNING, "Could not load image '%s': %s", source.c_str(), error.c_str());
                return 0;
            }

            // RmlUi expects premultiplied alpha; .ktex/.bmp store straight alpha.
            uint8_t *pixel = texture.pixels.data();
            const size_t pixelCount = size_t(texture.width) * texture.height;
            for (size_t i = 0; i < pixelCount; ++i, pixel += 4)
            {
                const unsigned alpha = pixel[3];
                pixel[0] = static_cast<uint8_t>((pixel[0] * alpha + 127) / 255);
                pixel[1] = static_cast<uint8_t>((pixel[1] * alpha + 127) / 255);
                pixel[2] = static_cast<uint8_t>((pixel[2] * alpha + 127) / 255);
            }

            dimensions = {static_cast<int>(texture.width), static_cast<int>(texture.height)};
            return static_cast<Rml::TextureHandle>(
                renderer->CreateUiTexture(dimensions.x, dimensions.y, texture.pixels.data()));
        }

        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override
        {
            KES_ZONE(UI, "UI generate texture");
            if (!renderer || source.size() < size_t(dimensions.x) * dimensions.y * 4u)
                return 0;
            return static_cast<Rml::TextureHandle>(renderer->CreateUiTexture(dimensions.x, dimensions.y, source.data()));
        }

        void ReleaseTexture(Rml::TextureHandle texture) override
        {
            if (renderer && texture)
                renderer->DestroyUiTexture(static_cast<UiTextureHandle>(texture));
        }

        void EnableScissorRegion(bool enable) override { m_scissorEnabled = enable; }

        void SetScissorRegion(Rml::Rectanglei region) override
        {
            m_scissor = {region.Left(), region.Top(), region.Width(), region.Height()};
        }

        void SetTransform(const Rml::Matrix4f *transform) override
        {
            m_hasTransform = transform != nullptr;
            if (transform)
                m_transform = *transform;
        }

    private:
        struct CompiledGeometry
        {
            std::vector<Rml::Vertex> vertices;
            std::vector<int> indices;
        };

        bool m_scissorEnabled = false;
        UiScissorRect m_scissor = {};
        bool m_hasTransform = false;
        Rml::Matrix4f m_transform = Rml::Matrix4f::Identity();
    };
}

struct UiSystem::Impl
{
    SDL_Window *window = nullptr;
    IRenderer *renderer = nullptr;
    std::unique_ptr<KesUiSystemInterface> systemInterface;
    KesUiFileInterface fileInterface;
    KesUiRenderInterface renderInterface;
    Rml::Context *context = nullptr;
    bool initialized = false;
    bool inputEnabled = false;
    bool debuggerInitialized = false;
    std::unordered_map<std::string, UiActionCallback> actions;

    struct ActionCall
    {
        std::string name;
        std::string args;
    };

    // onclick="name args; other args" -> one listener per attribute.
    class ActionListener final : public Rml::EventListener
    {
    public:
        ActionListener(Impl &owner, std::vector<ActionCall> calls) : m_owner(owner), m_calls(std::move(calls)) {}

        void ProcessEvent(Rml::Event &event) override
        {
            for (const ActionCall &call : m_calls)
            {
                auto it = m_owner.actions.find(call.name);
                if (it == m_owner.actions.end() || !it->second)
                {
                    Rml::Log::Message(Rml::Log::LT_WARNING, "No UI action registered for '%s' (%s event).",
                                      call.name.c_str(), event.GetType().c_str());
                    continue;
                }
                // Copy: the callback may replace or unregister itself.
                UiActionCallback callback = it->second;
                callback(event, call.args);
            }
        }

        void OnDetach(Rml::Element *) override { delete this; }

    private:
        Impl &m_owner;
        std::vector<ActionCall> m_calls;
    };

    class ActionInstancer final : public Rml::EventListenerInstancer
    {
    public:
        explicit ActionInstancer(Impl &owner) : m_owner(owner) {}

        Rml::EventListener *InstanceEventListener(const Rml::String &value, Rml::Element *) override
        {
            std::vector<ActionCall> calls;
            size_t start = 0;
            while (start <= value.size())
            {
                size_t end = value.find(';', start);
                if (end == Rml::String::npos)
                    end = value.size();
                const std::string statement = Trim(value.substr(start, end - start));
                if (!statement.empty())
                {
                    const size_t space = statement.find_first_of(" \t");
                    ActionCall call;
                    call.name = statement.substr(0, space);
                    if (space != std::string::npos)
                        call.args = Trim(statement.substr(space + 1));
                    calls.push_back(std::move(call));
                }
                start = end + 1;
            }
            if (calls.empty())
                return nullptr;
            return new ActionListener(m_owner, std::move(calls));
        }

    private:
        Impl &m_owner;
    };

    ActionInstancer actionInstancer{*this};

    struct OpenDocument
    {
        Rml::ElementDocument *document = nullptr;
        std::string path;   // empty for documents loaded from memory
    };
    std::vector<OpenDocument> documents;

    bool IsOpen(Rml::ElementDocument *document) const
    {
        if (!context || !document)
            return false;
        for (int i = 0; i < context->GetNumDocuments(); ++i)
        {
            if (context->GetDocument(i) == document)
                return true;
        }
        return false;
    }

    // Drops documents that RmlUi already closed (e.g. document->Close()).
    void PruneDocuments()
    {
        documents.erase(std::remove_if(documents.begin(), documents.end(),
                                       [this](const OpenDocument &open) { return !IsOpen(open.document); }),
                        documents.end());
    }
};

UiSystem::UiSystem() : m_impl(std::make_unique<Impl>()) {}

UiSystem::~UiSystem()
{
    Shutdown();
}

bool UiSystem::Init(SDL_Window *window, IRenderer *renderer, int pixelWidth, int pixelHeight)
{
    KES_ZONE(UI, "UI init");
    Impl &impl = *m_impl;
    if (impl.initialized)
        return true;
    if (!window || !renderer)
        return false;

    impl.window = window;
    impl.renderer = renderer;
    impl.renderInterface.renderer = renderer;
    impl.systemInterface = std::make_unique<KesUiSystemInterface>(window);

    Rml::SetSystemInterface(impl.systemInterface.get());
    Rml::SetFileInterface(&impl.fileInterface);
    Rml::SetRenderInterface(&impl.renderInterface);
    if (!Rml::Initialise())
    {
        EngineLog("[UI] RmlUi initialization failed.");
        impl.systemInterface.reset();
        return false;
    }
    Rml::Factory::RegisterEventListenerInstancer(&impl.actionInstancer);

    impl.context = Rml::CreateContext("main", {std::max(pixelWidth, 1), std::max(pixelHeight, 1)});
    if (!impl.context)
    {
        EngineLog("[UI] Could not create the RmlUi context.");
        Rml::Shutdown();
        impl.systemInterface.reset();
        return false;
    }
    impl.context->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(window));

    int fontCount = 0;
    for (const char *font : kEngineFonts)
    {
        if (Rml::LoadFontFace(font, fontCount == 0))
            ++fontCount;
    }
    if (fontCount == 0)
        EngineLog("[UI] Warning: no engine fonts found in engine/ui/fonts; text will not render until a font is loaded.");

#if ENGINE_ALLOW_DEBUG_MODE
    impl.debuggerInitialized = Rml::Debugger::Initialise(impl.context);
#endif

    impl.initialized = true;
    EngineLogf("[UI] Initialized RmlUi %s (%dx%d, %d font face(s)).", Rml::GetVersion().c_str(), pixelWidth, pixelHeight, fontCount);
    return true;
}

void UiSystem::Shutdown()
{
    Impl &impl = *m_impl;
    if (!impl.initialized)
        return;

    KES_ZONE(UI, "UI shutdown");
    impl.documents.clear();
    if (impl.debuggerInitialized)
    {
        Rml::Debugger::Shutdown();
        impl.debuggerInitialized = false;
    }
    // Releases contexts, documents, fonts and textures (through the renderer,
    // which must still be alive).
    Rml::Shutdown();
    impl.context = nullptr;
    impl.systemInterface.reset();
    impl.renderInterface.drawList.Clear();
    impl.renderInterface.renderer = nullptr;
    impl.renderer = nullptr;
    impl.window = nullptr;
    impl.actions.clear();
    impl.initialized = false;
    EngineLog("[UI] Shut down.");
}

bool UiSystem::IsInitialized() const
{
    return m_impl->initialized;
}

bool UiSystem::ProcessEvent(const SDL_Event &event)
{
    Impl &impl = *m_impl;
    if (!impl.initialized)
        return false;

    switch (event.type)
    {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        SetViewportSize(event.window.data1, event.window.data2);
        return false;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        SetDensityScale(SDL_GetWindowDisplayScale(impl.window));
        return false;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        impl.context->ProcessMouseLeave();
        return false;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        break;
    default:
        return false;
    }

    if (!impl.inputEnabled)
        return false;

    KES_ZONE(UI, "UI event");
    // Decided before the event changes hover/focus state.
    const bool pointerOverUi = IsPointerOverUi();
    SDL_Event copy = event;
    // RmlSDL returns false when the context handled the event (key bindings,
    // stopped propagation). For the mouse, RmlUi counts hovering any document
    // body as handled, so pointer consumption uses IsPointerOverUi instead.
    const bool propagated = RmlSDL::InputEventHandler(impl.context, impl.window, copy);

    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
        return pointerOverUi;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_INPUT:
        return WantsTextInput() || !propagated;
    default:
        // Motion and releases always reach the game too, so held buttons
        // and keys can't get stuck.
        return false;
    }
}

void UiSystem::SetInputEnabled(bool enabled)
{
    Impl &impl = *m_impl;
    if (impl.inputEnabled == enabled)
        return;
    impl.inputEnabled = enabled;
    if (!enabled && impl.context)
    {
        // Clear hover/active state and drop keyboard focus from text fields.
        impl.context->ProcessMouseLeave();
        if (Rml::Element *focus = impl.context->GetFocusElement())
            focus->Blur();
    }
}

bool UiSystem::IsInputEnabled() const
{
    return m_impl->inputEnabled;
}

void UiSystem::SetViewportSize(int pixelWidth, int pixelHeight)
{
    if (m_impl->context && pixelWidth > 0 && pixelHeight > 0)
        m_impl->context->SetDimensions({pixelWidth, pixelHeight});
}

void UiSystem::SetDensityScale(float scale)
{
    if (m_impl->context && scale > 0.0f)
        m_impl->context->SetDensityIndependentPixelRatio(scale);
}

void UiSystem::Update()
{
    if (!m_impl->initialized)
        return;
    KES_ZONE(UI, "UI update");
    m_impl->context->Update();
}

void UiSystem::Render()
{
    Impl &impl = *m_impl;
    if (!impl.initialized)
        return;
    {
        KES_ZONE(UI, "UI build draw list");
        impl.renderInterface.BeginFrame();
        impl.context->Render();
    }
    KES_ZONE_VALUE(UI, impl.renderInterface.drawList.commands.size());
    impl.renderer->RenderUi(impl.renderInterface.drawList);
}

Rml::ElementDocument *UiSystem::LoadDocument(const std::string &path, bool show)
{
    Impl &impl = *m_impl;
    if (!impl.initialized)
        return nullptr;
    KES_ZONE(UI, "UI load document");
    KES_ZONE_TEXT(UI, path.c_str(), path.size());
    Rml::ElementDocument *document = impl.context->LoadDocument(path);
    if (!document)
    {
        EngineLogf("[UI] Failed to load document: %s", path.c_str());
        return nullptr;
    }
    if (show)
        document->Show();
    impl.PruneDocuments();
    impl.documents.push_back({document, path});
    return document;
}

Rml::ElementDocument *UiSystem::LoadDocumentFromMemory(const std::string &rml, const std::string &sourceUrl, bool show)
{
    Impl &impl = *m_impl;
    if (!impl.initialized)
        return nullptr;
    Rml::ElementDocument *document = impl.context->LoadDocumentFromMemory(rml, sourceUrl);
    if (!document)
    {
        EngineLogf("[UI] Failed to load document from memory (%s).", sourceUrl.c_str());
        return nullptr;
    }
    if (show)
        document->Show();
    impl.PruneDocuments();
    impl.documents.push_back({document, {}});
    return document;
}

void UiSystem::CloseDocument(Rml::ElementDocument *document)
{
    Impl &impl = *m_impl;
    if (!impl.IsOpen(document))
        return;
    document->Close();
    impl.documents.erase(std::remove_if(impl.documents.begin(), impl.documents.end(),
                                        [document](const Impl::OpenDocument &open) { return open.document == document; }),
                         impl.documents.end());
}

void UiSystem::CloseAllDocuments()
{
    Impl &impl = *m_impl;
    impl.PruneDocuments();
    for (const Impl::OpenDocument &open : impl.documents)
        open.document->Close();
    impl.documents.clear();
}

void UiSystem::ReloadDocuments()
{
    Impl &impl = *m_impl;
    if (!impl.initialized)
        return;
    KES_ZONE(UI, "UI reload documents");
    impl.PruneDocuments();
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();

    std::vector<Impl::OpenDocument> previous = std::move(impl.documents);
    impl.documents.clear();
    for (Impl::OpenDocument &open : previous)
    {
        if (open.path.empty())
        {
            // Loaded from memory: there is no source file to reload from.
            impl.documents.push_back(open);
            continue;
        }
        const bool visible = open.document->IsVisible();
        open.document->Close();
        LoadDocument(open.path, visible);
    }
    EngineLogf("[UI] Reloaded %zu document(s).", previous.size());
}

bool UiSystem::LoadFontFace(const std::string &path, bool fallback)
{
    if (!m_impl->initialized)
        return false;
    return Rml::LoadFontFace(path, fallback);
}

void UiSystem::RegisterAction(const std::string &name, UiActionCallback callback)
{
    m_impl->actions[name] = std::move(callback);
}

void UiSystem::UnregisterAction(const std::string &name)
{
    m_impl->actions.erase(name);
}

bool UiSystem::WantsTextInput() const
{
    const Impl &impl = *m_impl;
    if (!impl.initialized || !impl.inputEnabled)
        return false;
    Rml::Element *focus = impl.context->GetFocusElement();
    if (!focus)
        return false;
    const Rml::String &tag = focus->GetTagName();
    if (tag == "textarea")
        return true;
    if (tag == "input")
    {
        const Rml::String type = focus->GetAttribute<Rml::String>("type", "text");
        return type == "text" || type == "password";
    }
    return false;
}

bool UiSystem::IsPointerOverUi() const
{
    const Impl &impl = *m_impl;
    if (!impl.initialized || !impl.inputEnabled)
        return false;
    // Hovering only the context root or a document's empty <body> background
    // doesn't count, so full-screen HUD documents don't swallow game clicks.
    Rml::Element *hover = impl.context->GetHoverElement();
    return hover && hover != impl.context->GetRootElement() && hover->GetTagName() != "body";
}

void UiSystem::SetDebuggerVisible(bool visible)
{
    if (m_impl->debuggerInitialized)
        Rml::Debugger::SetVisible(visible);
}

bool UiSystem::IsDebuggerVisible() const
{
    return m_impl->debuggerInitialized && Rml::Debugger::IsVisible();
}

Rml::Context *UiSystem::GetContext() const
{
    return m_impl->context;
}

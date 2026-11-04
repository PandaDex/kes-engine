#ifndef NOMINMAX
#define NOMINMAX   // file_paths.h includes <windows.h>
#endif
#include "ui_system.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementTabSet.h>
#include <RmlUi/Debugger.h>
#include <RmlUi_Platform_SDL.h>

#include "engine_configuration.h"
#include "file_paths.h"
#include "ini_file.h"
#include "input.h"
#include "logger.h"
#include "pak_file_system.h"
#include "profiler.h"
#include "texture_file.h"

namespace
{
    std::string Trim(const std::string &text)
    {
        const size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return {};
        const size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // "a, b, c" -> {"a", "b", "c"}
    std::vector<std::string> SplitList(const std::string &text)
    {
        std::vector<std::string> items;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find(',', start);
            if (end == std::string::npos)
                end = text.size();
            std::string item = Trim(text.substr(start, end - start));
            if (!item.empty())
                items.push_back(std::move(item));
            start = end + 1;
        }
        return items;
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

const char *const kUiDefaultActionMap = R"imap(
"UI_Up": { "NativeKeys": [ {"key": "Gamepad_DPad_Up"}, {"key": "Keyboard_Up"} ], "ValueType": "boolean" }
"UI_Down": { "NativeKeys": [ {"key": "Gamepad_DPad_Down"}, {"key": "Keyboard_Down"} ], "ValueType": "boolean" }
"UI_Left": { "NativeKeys": [ {"key": "Gamepad_DPad_Left"}, {"key": "Keyboard_Left"} ], "ValueType": "boolean" }
"UI_Right": { "NativeKeys": [ {"key": "Gamepad_DPad_Right"}, {"key": "Keyboard_Right"} ], "ValueType": "boolean" }
"UI_Navigate": { "NativeKeys": [ {"key": "Gamepad_LeftAxis_2D"} ], "ValueType": "vector2" }
"UI_Accept": { "NativeKeys": [ {"key": "Gamepad_Face_Bottom"}, {"key": "Keyboard_Return"}, {"key": "Keyboard_Keypad Enter"} ], "ValueType": "boolean" }
"UI_Back": { "NativeKeys": [ {"key": "Gamepad_Face_Right"}, {"key": "Gamepad_Special_Right"}, {"key": "Keyboard_Escape"} ], "ValueType": "boolean" }
"UI_PrevTab": { "NativeKeys": [ {"key": "Gamepad_LeftBumper"} ], "ValueType": "boolean" }
"UI_NextTab": { "NativeKeys": [ {"key": "Gamepad_RightBumper"} ], "ValueType": "boolean" }
"UI_Scroll": { "NativeKeys": [ {"key": "Gamepad_RightAxis_2D"} ], "ValueType": "vector2" }
)imap";

std::string GetDefaultUiConfigPath()
{
    return (GetExecutableDirectory() / "config" / "DefaultUi.ini").string();
}

UiConfig LoadUiConfig(const std::string &path, bool *outFound)
{
    UiConfig config;
    IniFile ini;
    const bool found = ini.LoadFromFile(path);
    if (outFound)
        *outFound = found;
    if (!found)
        return config;

    if (ini.Has("UI", "Fonts"))
        config.fonts = SplitList(ini.GetString("UI", "Fonts"));
    config.fallbackFonts = SplitList(ini.GetString("UI", "FallbackFonts"));
    config.densityScale = ini.GetFloat("UI", "DensityScale", config.densityScale);

    const std::string navigation = Lower(ini.GetString("Navigation", "GamepadNavigation", "uionly"));
    if (navigation == "never" || navigation == "false" || navigation == "off")
        config.navigation = UiNavigationMode::Never;
    else if (navigation == "gameandui")
        config.navigation = UiNavigationMode::GameAndUI;
    else
        config.navigation = UiNavigationMode::UIOnly;

    const std::string autoFocus = Lower(ini.GetString("Navigation", "AutoFocus", "gamepad"));
    if (autoFocus == "never" || autoFocus == "false" || autoFocus == "off")
        config.autoFocus = UiAutoFocus::Never;
    else if (autoFocus == "always")
        config.autoFocus = UiAutoFocus::Always;
    else
        config.autoFocus = UiAutoFocus::Gamepad;

    config.rememberFocus = ini.GetBool("Navigation", "RememberFocus", config.rememberFocus);
    config.hideCursorWithGamepad = ini.GetBool("Navigation", "HideCursorWithGamepad", config.hideCursorWithGamepad);
    config.repeatDelay = ini.GetFloat("Navigation", "RepeatDelay", config.repeatDelay);
    config.repeatInterval = ini.GetFloat("Navigation", "RepeatInterval", config.repeatInterval);
    config.stickPressThreshold = ini.GetFloat("Navigation", "StickPressThreshold", config.stickPressThreshold);
    config.stickReleaseThreshold = ini.GetFloat("Navigation", "StickReleaseThreshold", config.stickReleaseThreshold);
    config.scrollDeadzone = ini.GetFloat("Navigation", "ScrollDeadzone", config.scrollDeadzone);
    config.scrollSpeed = ini.GetFloat("Navigation", "ScrollSpeed", config.scrollSpeed);

    config.focusSound = ini.GetString("Sounds", "Focus");
    config.acceptSound = ini.GetString("Sounds", "Accept");
    config.backSound = ini.GetString("Sounds", "Back");
    return config;
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

    // Navigation with the UI_* input actions
    InputState *input = nullptr;
    UiConfig config;
    bool navigationEnabled = false;
    bool backPressed = false;
    bool confirmPressed = false;
    UiInputDevice device = UiInputDevice::Mouse;
    UiSoundCallback soundCallback;
    std::unordered_set<std::string> loadedFonts;

    enum NavDirection
    {
        Nav_None = -1,
        Nav_Up,
        Nav_Down,
        Nav_Left,
        Nav_Right,
        Nav_Count
    };
    int heldDirection = Nav_None;
    bool resyncHeld = true;       // next update adopts the held direction without moving
    bool repeatBlocked = false;   // held since navigation turned on: no repeat until it changes
    bool stickLatched = false;    // UI_Navigate hysteresis
    Uint64 nextRepeatNs = 0;
    Uint64 lastUpdateNs = 0;
    // Focus the first control of a tab panel once layout has run (after a tab switch).
    Rml::ObserverPtr<Rml::Element> pendingPanelFocus;
    std::unordered_map<Rml::ElementDocument *, Rml::ObserverPtr<Rml::Element>> rememberedFocus;

    bool NavigationActive() const { return initialized && inputEnabled && navigationEnabled && input; }

    // Directions, repeats and the stick latch start over, so a button held
    // while navigation turns on (e.g. Start opening the pause menu) does nothing.
    void ResetNavigationState()
    {
        heldDirection = Nav_None;
        resyncHeld = true;
        repeatBlocked = false;
        stickLatched = false;
        pendingPanelFocus.reset();
    }

    void PlaySound(const std::string &path) const
    {
        if (!path.empty() && soundCallback)
            soundCallback(path);
    }

    const InputActionState *ActionState(const char *name) const
    {
        if (!input)
            return nullptr;
        const auto found = input->actionStates.find(std::string_view(name));
        return found != input->actionStates.end() ? &found->second : nullptr;
    }
    bool ActionDown(const char *name) const
    {
        const InputActionState *state = ActionState(name);
        return state && state->down;
    }
    bool ActionPressed(const char *name) const
    {
        const InputActionState *state = ActionState(name);
        return state && state->pressed;
    }
    Vector2 ActionValue(const char *name) const
    {
        const InputActionState *state = ActionState(name);
        return state ? state->value2 : Vector2{};
    }

    void SetDevice(UiInputDevice newDevice)
    {
        if (device == newDevice)
            return;
        device = newDevice;
        // Hover styles would compete with the focus highlight.
        if (newDevice == UiInputDevice::Gamepad && context && inputEnabled)
            context->ProcessMouseLeave();
    }

    void TrackDevice(const SDL_Event &event)
    {
        switch (event.type)
        {
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            SetDevice(UiInputDevice::Gamepad);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (std::abs(static_cast<int>(event.gaxis.value)) > 16000)
                SetDevice(UiInputDevice::Gamepad);
            break;
        case SDL_EVENT_KEY_DOWN:
            SetDevice(UiInputDevice::Keyboard);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            // Ignore sensor jitter of a mouse nobody touches.
            if (std::fabs(event.motion.xrel) + std::fabs(event.motion.yrel) >= 2.0f)
                SetDevice(UiInputDevice::Mouse);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_FINGER_DOWN:
            SetDevice(UiInputDevice::Mouse);
            break;
        default:
            break;
        }
    }

    // <body class="input-mouse | input-keyboard | input-gamepad">
    void ApplyDeviceClasses()
    {
        for (int i = 0; i < context->GetNumDocuments(); ++i)
        {
            Rml::ElementDocument *document = context->GetDocument(i);
            document->SetClass("input-mouse", device == UiInputDevice::Mouse);
            document->SetClass("input-keyboard", device == UiInputDevice::Keyboard);
            document->SetClass("input-gamepad", device == UiInputDevice::Gamepad);
        }
    }

    static bool IsDebuggerDocument(const Rml::ElementDocument *document)
    {
        return document && document->GetId().rfind("rmlui-debug", 0) == 0;
    }

    static bool IsTabFocusable(const Rml::Element *element)
    {
        return element->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto;
    }

    static bool IsTextField(const Rml::Element *element)
    {
        const Rml::String &tag = element->GetTagName();
        if (tag == "textarea")
            return true;
        if (tag != "input")
            return false;
        const Rml::String type = element->GetAttribute<Rml::String>("type", "text");
        return type == "text" || type == "password";
    }

    static bool IsDescendantOf(const Rml::Element *element, const Rml::Element *ancestor)
    {
        for (; element; element = element->GetParentNode())
        {
            if (element == ancestor)
                return true;
        }
        return false;
    }

    // The focused element when it can be navigated from: visible, focusable
    // (or inside a focusable element) and not just a document's <body>.
    Rml::Element *NavigableFocus() const
    {
        Rml::Element *focus = context->GetFocusElement();
        if (!focus || !focus->IsVisible(true) || IsDebuggerDocument(focus->GetOwnerDocument()))
            return nullptr;
        for (Rml::Element *element = focus; element; element = element->GetParentNode())
        {
            if (element == element->GetOwnerDocument())
                return nullptr;
            if (IsTabFocusable(element))
                return focus;
        }
        return nullptr;
    }

    // First control inside `scope` (a document or an element in it) that may
    // get focus without being asked for: text fields are skipped, so a menu
    // never starts in text-entry mode.
    static Rml::Element *FirstAutoFocusable(Rml::ElementDocument *document, Rml::Element *scope)
    {
        Rml::Element *element = document->FindNextTabElement(scope, true, false);
        for (int guard = 0; element && guard < 4096; ++guard)
        {
            if (element == document || !IsDescendantOf(element, scope))
                return nullptr;
            if (!IsTextField(element))
                return element;
            element = document->FindNextTabElement(element, true, false);
        }
        return nullptr;
    }

    // The document navigation acts on: the one holding focus (ElementDocument::Show
    // focuses the shown document), else the top-most visible one with a control.
    Rml::ElementDocument *NavigationDocument() const
    {
        if (Rml::Element *focus = context->GetFocusElement())
        {
            Rml::ElementDocument *document = focus->GetOwnerDocument();
            if (document && document->IsVisible() && !IsDebuggerDocument(document))
                return document;
        }
        for (int i = context->GetNumDocuments() - 1; i >= 0; --i)
        {
            Rml::ElementDocument *document = context->GetDocument(i);
            if (document && document->IsVisible() && !IsDebuggerDocument(document) && FirstAutoFocusable(document, document))
                return document;
        }
        return nullptr;
    }

    static bool FocusVisible(Rml::Element *element)
    {
        if (!element || !element->Focus(true))
            return false;
        element->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
        return true;
    }

    // The remembered control of `document` if it is still usable, else its
    // first control.
    bool FocusDefault(Rml::ElementDocument *document)
    {
        if (!document)
            return false;
        if (config.rememberFocus)
        {
            const auto found = rememberedFocus.find(document);
            Rml::Element *element = found != rememberedFocus.end() ? found->second.get() : nullptr;
            if (element && element->GetOwnerDocument() == document && element->IsVisible(true) && IsTabFocusable(element))
                return FocusVisible(element);
        }
        return FocusVisible(FirstAutoFocusable(document, document));
    }

    // Sends a key press through the context; returns true when it propagated
    // (nothing used it).
    bool SendKey(Rml::Input::KeyIdentifier key)
    {
        const bool propagated = context->ProcessKeyDown(key, 0);
        context->ProcessKeyUp(key, 0);
        return propagated;
    }

    void Navigate(int direction)
    {
        Rml::Element *focus = NavigableFocus();
        if (!focus)
        {
            if (FocusDefault(NavigationDocument()))
                PlaySound(config.focusSound);
            return;
        }

        // Up/down in a text field would only move the caret: leave the field
        // along the tab order instead (keyboard bindings are off while typing,
        // so this is the gamepad).
        if (IsTextField(focus) && (direction == Nav_Up || direction == Nav_Down))
        {
            Rml::ElementDocument *document = focus->GetOwnerDocument();
            if (FocusVisible(document->FindNextTabElement(focus, direction == Nav_Down, false)))
                PlaySound(config.focusSound);
            return;
        }

        static constexpr Rml::Input::KeyIdentifier kKeys[Nav_Count] = {
            Rml::Input::KI_UP, Rml::Input::KI_DOWN, Rml::Input::KI_LEFT, Rml::Input::KI_RIGHT};
        SendKey(kKeys[direction]);
        if (context->GetFocusElement() != focus)
            PlaySound(config.focusSound);
    }

    void Accept()
    {
        if (NavigableFocus())
        {
            if (!SendKey(Rml::Input::KI_RETURN))
                PlaySound(config.acceptSound);
        }
        else if (FocusDefault(NavigationDocument()))
            PlaySound(config.focusSound);
        else
            confirmPressed = true;
    }

    // Closes the focused <select>'s drop-down; RmlUi has no Escape handling for it.
    bool CloseOpenSelect()
    {
        Rml::Element *focus = context->GetFocusElement();
        auto *select = focus ? rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(focus) : nullptr;
        if (!select || !select->IsSelectBoxVisible())
            return false;
        select->HideSelectBox();
        return true;
    }

    void Back()
    {
        PlaySound(config.backSound);
        if (CloseOpenSelect())
            return;
        if (SendKey(Rml::Input::KI_ESCAPE))
            backPressed = true;
    }

    void SwitchTab(int step)
    {
        Rml::ElementDocument *document = NavigationDocument();
        if (!document)
            return;
        Rml::ElementList tabsets;
        document->GetElementsByTagName(tabsets, "tabset");
        for (Rml::Element *element : tabsets)
        {
            auto *tabset = rmlui_dynamic_cast<Rml::ElementTabSet *>(element);
            if (!tabset || !tabset->IsVisible(true) || tabset->GetNumTabs() <= 1)
                continue;
            const int count = tabset->GetNumTabs();
            const int active = ((tabset->GetActiveTab() + step) % count + count) % count;
            tabset->SetActiveTab(active);
            PlaySound(config.focusSound);

            // Focus leaves the hidden panel; the new panel's first control
            // gets it after the next layout pass (ApplyPendingPanelFocus).
            document->Focus();
            pendingPanelFocus.reset();
            for (int i = 0; i < tabset->GetNumChildren(); ++i)
            {
                Rml::Element *panels = tabset->GetChild(i);
                if (panels->GetTagName() == "panels" && active < panels->GetNumChildren())
                    pendingPanelFocus = panels->GetChild(active)->GetObserverPtr();
            }
            return;
        }
    }

    // Nearest ancestor of `element` (inclusive) that can scroll vertically.
    static Rml::Element *ScrollContainerOf(Rml::Element *element)
    {
        for (; element; element = element->GetParentNode())
        {
            const auto overflow = element->GetComputedValues().overflow_y();
            const bool scrolls = overflow == Rml::Style::Overflow::Auto || overflow == Rml::Style::Overflow::Scroll;
            if (scrolls && element->GetScrollHeight() > element->GetClientHeight() + 1.0f)
                return element;
        }
        return nullptr;
    }

    // Breadth-first search for the first visible scrollable element under `root`.
    static Rml::Element *FindScrollContainer(Rml::Element *root)
    {
        std::vector<Rml::Element *> queue = {root};
        for (size_t i = 0; i < queue.size(); ++i)
        {
            Rml::Element *element = queue[i];
            if (!element->IsVisible())
                continue;
            if (element != root && ScrollContainerOf(element) == element)
                return element;
            for (int c = 0; c < element->GetNumChildren(); ++c)
                queue.push_back(element->GetChild(c));
        }
        return nullptr;
    }

    void Scroll(float amount)
    {
        Rml::Element *focus = NavigableFocus();
        Rml::Element *target = focus ? ScrollContainerOf(focus) : nullptr;
        if (!target)
        {
            Rml::ElementDocument *document = focus ? focus->GetOwnerDocument() : NavigationDocument();
            target = document ? FindScrollContainer(document) : nullptr;
        }
        if (target)
            target->SetScrollTop(target->GetScrollTop() + amount);
    }

    int CurrentDirection()
    {
        static constexpr const char *kDirectionActions[Nav_Count] = {"UI_Up", "UI_Down", "UI_Left", "UI_Right"};
        for (int direction = 0; direction < Nav_Count; ++direction)
        {
            if (ActionDown(kDirectionActions[direction]))
                return direction;
        }
        const Vector2 stick = ActionValue("UI_Navigate");
        const float magnitude = std::max(std::fabs(stick.x), std::fabs(stick.y));
        stickLatched = magnitude >= (stickLatched ? config.stickReleaseThreshold : config.stickPressThreshold);
        if (!stickLatched)
            return Nav_None;
        if (std::fabs(stick.x) > std::fabs(stick.y))
            return stick.x < 0.0f ? Nav_Left : Nav_Right;
        return stick.y < 0.0f ? Nav_Up : Nav_Down;   // SDL: +y is down
    }

    // UI action input, once per frame before layout.
    void UpdateNavigation(Uint64 nowNs, float dt)
    {
        if (!NavigationActive())
            return;

        const int direction = CurrentDirection();
        const auto seconds = [](float value) { return static_cast<Uint64>(std::max(value, 0.0f) * 1e9f); };
        if (resyncHeld)
        {
            resyncHeld = false;
            heldDirection = direction;
            repeatBlocked = direction != Nav_None;
        }
        else if (direction != heldDirection)
        {
            heldDirection = direction;
            repeatBlocked = false;
            if (direction != Nav_None)
            {
                Navigate(direction);
                nextRepeatNs = nowNs + seconds(config.repeatDelay);
            }
        }
        else if (direction != Nav_None && !repeatBlocked && nowNs >= nextRepeatNs)
        {
            Navigate(direction);
            nextRepeatNs = nowNs + seconds(std::max(config.repeatInterval, 0.01f));
        }

        if (ActionPressed("UI_Accept"))
            Accept();
        if (ActionPressed("UI_Back"))
            Back();
        if (ActionPressed("UI_PrevTab"))
            SwitchTab(-1);
        if (ActionPressed("UI_NextTab"))
            SwitchTab(1);

        const float scroll = ActionValue("UI_Scroll").y;
        if (std::fabs(scroll) > config.scrollDeadzone)
            Scroll(scroll * config.scrollSpeed * context->GetDensityIndependentPixelRatio() * dt);
    }

    // After layout: keeps a usable focus while navigating and remembers it.
    void UpdateFocus()
    {
        for (auto it = rememberedFocus.begin(); it != rememberedFocus.end();)
            it = (IsOpen(it->first) && it->second) ? std::next(it) : rememberedFocus.erase(it);

        if (!NavigationActive())
            return;

        if (Rml::Element *panel = pendingPanelFocus.get())
        {
            pendingPanelFocus.reset();
            if (Rml::ElementDocument *document = panel->GetOwnerDocument())
                FocusVisible(FirstAutoFocusable(document, panel));
        }

        if (Rml::Element *focus = NavigableFocus())
        {
            rememberedFocus[focus->GetOwnerDocument()] = focus->GetObserverPtr();
            return;
        }
        // Nothing usable has focus: a document was just shown, or the focused
        // control was hidden or removed (data-if, data-for...).
        const bool autoFocus = config.autoFocus == UiAutoFocus::Always ||
                               (config.autoFocus == UiAutoFocus::Gamepad && device == UiInputDevice::Gamepad);
        if (autoFocus)
            FocusDefault(NavigationDocument());
    }

    void LoadFonts()
    {
        int index = 0;
        for (const std::string &font : config.fonts)
        {
            const bool fallback = index++ == 0;
            if (loadedFonts.insert(font).second && !Rml::LoadFontFace(font, fallback))
                loadedFonts.erase(font);
        }
        for (const std::string &font : config.fallbackFonts)
        {
            if (loadedFonts.insert(font).second && !Rml::LoadFontFace(font, true))
                loadedFonts.erase(font);
        }
    }

    void ApplyDisplayScale()
    {
        const float display = window ? SDL_GetWindowDisplayScale(window) : 1.0f;
        const float scale = (display > 0.0f ? display : 1.0f) * (config.densityScale > 0.0f ? config.densityScale : 1.0f);
        context->SetDensityIndependentPixelRatio(scale);
    }

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

bool UiSystem::Init(SDL_Window *window, IRenderer *renderer, int pixelWidth, int pixelHeight,
                    InputState *input, const UiConfig &config)
{
    KES_ZONE(UI, "UI init");
    Impl &impl = *m_impl;
    if (impl.initialized)
        return true;
    if (!window || !renderer)
        return false;

    impl.window = window;
    impl.renderer = renderer;
    impl.input = input;
    impl.config = config;
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
    impl.ApplyDisplayScale();

    impl.LoadFonts();
    const int fontCount = static_cast<int>(impl.loadedFonts.size());
    if (fontCount == 0)
        EngineLog("[UI] Warning: no UI fonts loaded (DefaultUi.ini [UI] Fonts); text will not render until a font is loaded.");

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
    impl.loadedFonts.clear();
    impl.rememberedFocus.clear();
    impl.ResetNavigationState();
    impl.input = nullptr;
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

    impl.TrackDevice(event);

    switch (event.type)
    {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        SetViewportSize(event.window.data1, event.window.data2);
        return false;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        impl.ApplyDisplayScale();
        return false;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        impl.context->ProcessMouseLeave();
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        impl.context->ProcessMouseLeave();
        impl.ResetNavigationState();
        return false;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!impl.inputEnabled)
            return false;
        // Keys bound to UI actions (arrows, Enter, Escape by default) are
        // handled by the actions, with the same repeat timing as the gamepad,
        // except while typing. They still reach game input, which is what the
        // UI actions read.
        if (impl.NavigationActive() && !WantsTextInput() &&
            InputIsKeyBound(*impl.input, event.key.scancode, InputActionContext::UI))
            return false;
        // Escape closes an open <select> drop-down (RmlUi doesn't).
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE && impl.CloseOpenSelect())
            return true;
        break;
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
        break;
    default:
        // Gamepads are read through the UI_* input actions.
        return false;
    }

    if (!impl.inputEnabled)
        return false;

    KES_ZONE(UI, "UI event");
    // Decided before the event changes hover/focus state.
    const bool pointerOverUi = IsPointerOverUi();
    const bool textInput = WantsTextInput();
    SDL_Event copy = event;
    // RmlSDL returns false when the context handled the event (key bindings,
    // stopped propagation). For the mouse, RmlUi counts hovering any document
    // body as handled, so pointer consumption uses IsPointerOverUi instead.
    const bool propagated = RmlSDL::InputEventHandler(impl.context, impl.window, copy);

    // Escape that RmlUi didn't use while navigation (UI_Back) is off.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE && !event.key.repeat &&
        propagated && !textInput)
        impl.backPressed = true;

    // Clicks and typing change focus; UI action keyboard bindings must not
    // fire while a text field has it.
    if (impl.input)
        impl.input->uiTextInput = WantsTextInput();

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && pointerOverUi)
    {
        for (Rml::Element *element = impl.context->GetHoverElement(); element; element = element->GetParentNode())
        {
            if (element == element->GetOwnerDocument())
                break;
            if (Impl::IsTabFocusable(element))
            {
                impl.PlaySound(impl.config.acceptSound);
                break;
            }
        }
    }

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
    impl.ResetNavigationState();
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

void UiSystem::SetNavigationEnabled(bool enabled)
{
    Impl &impl = *m_impl;
    if (impl.navigationEnabled == enabled)
        return;
    impl.navigationEnabled = enabled;
    impl.ResetNavigationState();
}

bool UiSystem::IsNavigationEnabled() const
{
    return m_impl->navigationEnabled;
}

UiInputDevice UiSystem::GetActiveInputDevice() const
{
    return m_impl->device;
}

void UiSystem::SetSoundCallback(UiSoundCallback callback)
{
    m_impl->soundCallback = std::move(callback);
}

void UiSystem::SetConfig(const UiConfig &config)
{
    Impl &impl = *m_impl;
    const bool densityChanged = config.densityScale != impl.config.densityScale;
    impl.config = config;
    impl.ResetNavigationState();
    if (!impl.initialized)
        return;
    impl.LoadFonts();
    if (densityChanged)
        impl.ApplyDisplayScale();
}

const UiConfig &UiSystem::GetConfig() const
{
    return m_impl->config;
}

bool UiSystem::WasBackPressed() const
{
    return m_impl->backPressed;
}

bool UiSystem::WasConfirmPressed() const
{
    return m_impl->confirmPressed;
}

void UiSystem::BeginFrame()
{
    m_impl->backPressed = false;
    m_impl->confirmPressed = false;
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
    Impl &impl = *m_impl;
    const Uint64 now = SDL_GetTicksNS();
    const float dt = impl.lastUpdateNs ? std::min(0.1f, static_cast<float>(now - impl.lastUpdateNs) * 1e-9f) : 0.0f;
    impl.lastUpdateNs = now;
    impl.UpdateNavigation(now, dt);
    impl.ApplyDeviceClasses();
    impl.context->Update();
    impl.UpdateFocus();
    if (impl.input)
        impl.input->uiTextInput = WantsTextInput();
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

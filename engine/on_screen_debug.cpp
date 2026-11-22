#include "on_screen_debug.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include <RmlUi/Core.h>
#include "engine_configuration.h"
#include "ui_system.h"

namespace
{
    constexpr size_t kMaxMessages = 40;
    constexpr const char* kDocumentId = "kes-on-screen-debug";

    constexpr const char* kDocumentRml = R"(<rml>
<head>
    <title>On-screen debug</title>
    <link type="text/rcss" href="/engine/ui/kes.rcss"/>
    <style>
        body
        {
            pointer-events: none;
            padding: 10dp;
            box-sizing: border-box;
            z-index: 1000;
        }
        .row
        {
            display: block;
            pointer-events: none;
            margin-bottom: 10dp;
        }
        /* inline-block: as wide as its text, wrapping at the screen edge. No
           percentage max-width: RmlUi resolves it to 0 while measuring the text. */
        .message
        {
            display: inline-block;
            pointer-events: none;
            padding: 4dp 6dp;
            line-height: 24dp;
            font-size: 15dp;
            color: #ffffff;
        }
        .info    { border-left: 4dp #1573ff; background-color: #1573ff50; }
        .warning { border-left: 4dp #ff9a15; background-color: #ff9a1559; }
        .error   { border-left: 4dp #ff1515; background-color: #ff151550; }
    </style>
</head>
<body id="kes-on-screen-debug"></body>
</rml>)";

    struct Message
    {
        int64_t key = -1;
        DebugMessageType type = DebugMessageType::Info;
        std::string text;
        float remaining = 0.0f;
        bool shown = false; // drawn at least once; 0-second messages last one frame
    };

    struct State
    {
        std::mutex mutex;
        std::vector<Message> messages; // oldest first
        bool dirty = false;
        Rml::ElementDocument* document = nullptr;
    };

    State& GetState()
    {
        static State state;
        return state;
    }

    std::string EscapeRml(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (char c : text)
        {
            switch (c)
            {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\n': out += "<br/>"; break;
            default:   out += c; break;
            }
        }
        return out;
    }

    const char* TypeClass(DebugMessageType type)
    {
        switch (type)
        {
        case DebugMessageType::Warning: return "warning";
        case DebugMessageType::Error:   return "error";
        default:                        return "info";
        }
    }
}

void AddOnScreenDebugMessage(int64_t key, float seconds, DebugMessageType type, std::string_view message)
{
#if ENGINE_ALLOW_DEBUG_MODE
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    Message* target = nullptr;
    if (key != -1)
    {
        for (size_t i = 0; i < state.messages.size(); ++i)
        {
            if (state.messages[i].key == key)
            {
                target = &state.messages[i];
                break;
            }
        }
    }
    if (!target)
    {
        if (state.messages.size() >= kMaxMessages)
            state.messages.erase(state.messages.begin());
        state.messages.emplace_back();
        target = &state.messages.back();
        target->key = key;
        state.dirty = true;
    }
    else if (target->text != message || target->type != type)
    {
        state.dirty = true;
    }
    target->type = type;
    target->text.assign(message);
    target->remaining = seconds > 0.0f ? seconds : 0.0f;
    target->shown = false;
#else
    (void)key;
    (void)seconds;
    (void)type;
    (void)message;
#endif
}

void AddOnScreenDebugMessagef(int64_t key, float seconds, DebugMessageType type, const char* format, ...)
{
#if ENGINE_ALLOW_DEBUG_MODE
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format ? format : "", args);
    va_end(args);
    AddOnScreenDebugMessage(key, seconds, type, buffer);
#else
    (void)key;
    (void)seconds;
    (void)type;
    (void)format;
#endif
}

void RemoveOnScreenDebugMessage(int64_t key)
{
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    const size_t before = state.messages.size();
    std::erase_if(state.messages, [key](const Message& message)
        { return message.key == key; });
    state.dirty = state.dirty || state.messages.size() != before;
}

void ClearOnScreenDebugMessages()
{
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dirty = state.dirty || !state.messages.empty();
    state.messages.clear();
}

void OnScreenDebugUpdate(UiSystem& ui, float dt)
{
#if ENGINE_ALLOW_DEBUG_MODE
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);

    // Messages already drawn age; those out of time go.
    const size_t before = state.messages.size();
    for (Message& message : state.messages)
    {
        if (message.shown)
            message.remaining -= dt;
    }
    std::erase_if(state.messages, [](const Message& message)
        { return message.shown && message.remaining <= 0.0f; });
    if (state.messages.size() != before)
        state.dirty = true;

    if (!ui.IsInitialized())
        return;
    Rml::Context* context = ui.GetContext();
    // Game code may have closed every document.
    if (state.document && context->GetDocument(kDocumentId) != state.document)
        state.document = nullptr;
    if (!state.document)
    {
        if (state.messages.empty())
            return;
        state.document = ui.LoadDocumentFromMemory(kDocumentRml, "[on-screen debug]");
        if (!state.document)
            return;
        state.dirty = true;
    }

    if (state.dirty)
    {
        std::string rml;
        for (auto it = state.messages.rbegin(); it != state.messages.rend(); ++it)
            rml += std::string("<div class=\"row\"><div class=\"message ") + TypeClass(it->type) + "\">" + EscapeRml(it->text) + "</div></div>";
        state.document->SetInnerRML(rml);
        state.dirty = false;
    }
    for (Message& message : state.messages)
        message.shown = true;

    if (!state.messages.empty())
        state.document->PullToFront(); // over documents shown later
#else
    (void)ui;
    (void)dt;
#endif
}

void OnScreenDebugShutdown(UiSystem& ui)
{
    State& state = GetState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.document && ui.IsInitialized() && ui.GetContext()->GetDocument(kDocumentId) == state.document)
        ui.CloseDocument(state.document);
    state.document = nullptr;
    state.messages.clear();
    state.dirty = false;
}

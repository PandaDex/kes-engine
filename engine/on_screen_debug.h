#pragma once
#include <cstdint>
#include <string_view>

class UiSystem;

enum class DebugMessageType
{
    Info,
    Warning,
    Error
};

// On-screen debug messages in the top-left corner, like Unreal's
// GEngine->AddOnScreenDebugMessage. Newest on top.
//   key      -1 adds a new line; any other value replaces the line with that
//            key (for values printed every frame)
//   seconds  how long it stays; 0 = this frame only
// Callable from any thread. Debug builds only (ENGINE_ALLOW_DEBUG_MODE):
// elsewhere these do nothing.
void AddOnScreenDebugMessage(int64_t key, float seconds, DebugMessageType type, std::string_view message);
void AddOnScreenDebugMessagef(int64_t key, float seconds, DebugMessageType type, const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
void RemoveOnScreenDebugMessage(int64_t key);
void ClearOnScreenDebugMessages();

// Engine plumbing: ages the messages and updates their document (before UiSystem::Update).
void OnScreenDebugUpdate(UiSystem& ui, float dt);
void OnScreenDebugShutdown(UiSystem& ui);

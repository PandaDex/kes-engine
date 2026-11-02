#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include "engine_types.h"
#include <vector>
#include "reflection.h"

enum class InputActionValueType
{
    Boolean,
    Float,
    Vector2,
    Vector3
};

enum class InputNativeKeyKind
{
    Unknown,
    Keyboard,
    MouseButton,
    Mouse2DAxis,
    Mouse_Y_Axis,
    Mouse_X_Axis,
    GamepadButton,
    GamepadLeftAxis2D,
    GamepadRightAxis2D,
    GamepadAxis
};

K_ENUM()
enum class InputModifierKind
{
    Deadzone,
    Negate,
    Swizzle
};

K_STRUCT()
struct InputModifier
{
    InputModifierKind kind = InputModifierKind::Deadzone;
    float scalarValue = 0.0f;
    int swizzle[3] = { 0, 1, 2 };
    bool negateX = false;
    bool negateY = false;
    bool negateZ = false;
};

struct InputBinding
{
    InputNativeKeyKind keyKind = InputNativeKeyKind::Unknown;
    SDL_Scancode keyboardKey = SDL_SCANCODE_UNKNOWN;
    SDL_GamepadButton gamepadButton = SDL_GAMEPAD_BUTTON_INVALID;
    SDL_GamepadAxis gamepadAxis = SDL_GAMEPAD_AXIS_INVALID;
    std::vector<InputModifier> modifiers;
    bool playerAssignable = false;
    std::string assignableKey;
};

struct InputAction
{
    std::string name;
    InputActionValueType valueType = InputActionValueType::Boolean;
    std::vector<InputBinding> bindings;
};

struct InputActionState
{
    bool down = false;
    bool pressed = false;
    bool released = false;

    Vector2 value2 = {};
    Vector3 value3 = {};
};

// Transparent hash so action lookups by const char* / string_view don't
// allocate a temporary std::string every call.
struct InputStringHash
{
    using is_transparent = void;
    size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};

struct InputState
{
    bool keys[SDL_SCANCODE_COUNT] = {};
    bool previousKeys[SDL_SCANCODE_COUNT] = {};

    bool mouseButtons[8] = {};
    bool previousMouseButtons[8] = {};
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;

    SDL_Gamepad* gamepad = nullptr;

    std::vector<InputAction> actions;
    std::unordered_map<std::string, InputActionState, InputStringHash, std::equal_to<>> actionStates;

    // Set by the engine's input mode (EngineSetInputMode):
    //   gameInputEnabled = false -> actions read as idle (UI-only mode)
    //   mouseLookEnabled = false -> mouse motion doesn't feed Mouse2DAxis bindings
    //                               (the cursor is free, so its motion isn't "look")
    bool gameInputEnabled = true;
    bool gameInputWasEnabled = true;   // last InputEvaluateActions: suppresses "pressed" for keys held while re-enabling
    bool mouseLookEnabled = true;
};

void InputInit(InputState& input);
void InputShutdown(InputState& input);
void InputBeginFrame(InputState& input);
void InputProcessEvent(InputState& input, const SDL_Event& e);
void InputEndFrame(InputState& input);
// Releases every held key/button, e.g. when the window loses focus and the
// key-up events will never arrive.
void InputResetState(InputState& input);

bool InputLoadActionMap(InputState& input, const char* path, std::string* outError = nullptr);
void InputEvaluateActions(InputState& input);

bool InputActionDown(const InputState& input, const char* actionName);
bool InputActionPressed(const InputState& input, const char* actionName);
bool InputActionReleased(const InputState& input, const char* actionName);
Vector2 InputActionValue2D(const InputState& input, const char* actionName);
Vector3 InputActionValue3D(const InputState& input, const char* actionName);

bool InputKeyDown(const InputState& input, SDL_Scancode key);
bool InputKeyPressed(const InputState& input, SDL_Scancode key);
bool InputKeyReleased(const InputState& input, SDL_Scancode key);
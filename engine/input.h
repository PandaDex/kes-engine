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
    GamepadAxis,
    MouseWheelUp, // one frame per wheel step
    MouseWheelDown
};

// Which kind of device the player used last (button prompts, cursor).
enum class InputDevice
{
    KeyboardMouse,
    Gamepad
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
    uint8_t mouseButton = 0; // SDL_BUTTON_LEFT ...
    std::vector<InputModifier> modifiers;
    std::string key; // current key name, e.g. "Keyboard_Space"
    std::string defaultKey; // the key the input map gives it
    bool playerAssignable = false;
    std::string assignableKey;
};

// Which input mode an action is read in. Game actions read as idle while game
// input is off (InputMode::UIOnly); UI actions (menu navigation, see
// ui_system.h) only read while UI navigation is on. Set with "Context": "UI"
// in the input map; actions named UI_* default to UI.
enum class InputActionContext
{
    Game,
    UI
};

struct InputAction
{
    std::string name;
    InputActionValueType valueType = InputActionValueType::Boolean;
    InputActionContext context = InputActionContext::Game;
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
    bool keysTapped[SDL_SCANCODE_COUNT] = {}; // went down this frame, so a press and release in one frame still counts

    bool mouseButtons[8] = {};
    bool previousMouseButtons[8] = {};
    bool mouseButtonsTapped[8] = {};
    float mouseWheel = 0.0f; // steps this frame, positive = away from the player
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;

    // Every connected gamepad is opened; `gamepad` is the one used last (the
    // one bindings read), so any controller can take over at any time.
    SDL_Gamepad* gamepad = nullptr;
    std::vector<SDL_Gamepad*> gamepads;

    std::vector<InputAction> actions;
    std::unordered_map<std::string, InputActionState, InputStringHash, std::equal_to<>> actionStates;

    // Set by the engine's input mode (EngineSetInputMode):
    //   gameInputEnabled = false -> actions read as idle (UI-only mode)
    //   mouseLookEnabled = false -> mouse motion doesn't feed Mouse2DAxis bindings
    //                               (the cursor is free, so its motion isn't "look")
    bool gameInputEnabled = true;
    bool gameInputWasEnabled = true; // last InputEvaluateActions: suppresses "pressed" for keys held while re-enabling
    bool mouseLookEnabled = true;

    // Set by the engine / UI system for InputActionContext::UI actions:
    //   uiActionsEnabled = false -> UI actions read as idle
    //   uiTextInput      = true  -> their keyboard bindings read as idle (typing
    //                               into a text field must not navigate)
    bool uiActionsEnabled = false;
    bool uiActionsWasEnabled = false;
    bool uiTextInput = false;

    InputDevice activeDevice = InputDevice::KeyboardMouse;

    // Key capture for rebinding menus (InputBeginKeyCapture).
    bool captureActive = false;
    InputDevice captureDevice = InputDevice::KeyboardMouse;
    std::string capturedKey;
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
// Parses input map text and adds the actions the loaded map doesn't define,
// e.g. engine defaults that projects may override in their own map.
bool InputAddDefaultActions(InputState& input, std::string_view mapText, std::string* outError = nullptr);
// True when a keyboard binding of an action in `context` uses `key`.
bool InputIsKeyBound(const InputState& input, SDL_Scancode key, InputActionContext context);
void InputEvaluateActions(InputState& input);

bool InputActionDown(const InputState& input, const char* actionName);
bool InputActionPressed(const InputState& input, const char* actionName);
bool InputActionReleased(const InputState& input, const char* actionName);
Vector2 InputActionValue2D(const InputState& input, const char* actionName);
Vector3 InputActionValue3D(const InputState& input, const char* actionName);

InputDevice InputGetActiveDevice(const InputState& input);
// Layout of the active gamepad, for button glyphs (SDL_GAMEPAD_TYPE_UNKNOWN without one).
SDL_GamepadType InputGetGamepadType(const InputState& input);

// Rebinding. Only bindings marked "bPlayerAssignable" in the input map can be
// changed, to a key of the same device and shape (a button for a button, a
// stick for a stick); modifiers stay. Key names are input map names
// ("Keyboard_F", "Mouse_Right", "Gamepad_Face_Left"). Overrides are kept by
// action and default key, so they survive edits to the rest of the map.
struct InputBindingInfo
{
    size_t index = 0; // for InputRebind
    std::string key;
    std::string defaultKey;
    InputDevice device = InputDevice::KeyboardMouse;
    bool playerAssignable = false;
};
std::vector<InputBindingInfo> InputGetBindings(const InputState& input, const char* actionName);
bool InputRebind(InputState& input, const char* actionName, size_t bindingIndex, const char* keyName, std::string* outError = nullptr);
void InputResetBinding(InputState& input, const char* actionName, size_t bindingIndex);
void InputResetAllBindings(InputState& input);
// Actions of `context` that have a binding on `keyName` (to warn about or swap duplicates).
std::vector<std::string> InputFindActionsUsingKey(const InputState& input, const char* keyName,
    InputActionContext context = InputActionContext::Game);
// Player overrides file; empty path = <user data>/Config/InputBindings.ini.
// The engine loads it at startup; call it again after InputLoadActionMap.
bool InputSaveBindingOverrides(const InputState& input, const char* path = nullptr, std::string* outError = nullptr);
bool InputLoadBindingOverrides(InputState& input, const char* path = nullptr, std::string* outError = nullptr);

// "Press a key" for rebinding: the next button press of `device` is captured
// (and doesn't reach actions) until InputGetCapturedKey returns it once.
void InputBeginKeyCapture(InputState& input, InputDevice device);
void InputCancelKeyCapture(InputState& input);
bool InputIsCapturingKey(const InputState& input);
bool InputGetCapturedKey(InputState& input, std::string& outKeyName);

// Text for button prompts: keyboard keys follow the keyboard layout, gamepad
// buttons the active pad (A / Cross, LB / L1 ...).
std::string InputGetKeyDisplayName(const InputState& input, const std::string& keyName);
// First binding of the action on `device`, as display text; empty when none.
std::string InputGetActionDisplayName(const InputState& input, const char* actionName, InputDevice device);

// Vibration on the active gamepad, 0-1 strengths. False without a pad that
// supports it or when GameSettings turns vibration off.
bool InputRumble(InputState& input, float lowFrequency, float highFrequency, float seconds);
bool InputRumbleTriggers(InputState& input, float left, float right, float seconds);
void InputStopRumble(InputState& input);

bool InputKeyDown(const InputState& input, SDL_Scancode key);
bool InputKeyPressed(const InputState& input, SDL_Scancode key);
bool InputKeyReleased(const InputState& input, SDL_Scancode key);

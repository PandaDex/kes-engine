#include "input.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "logger.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "file_paths.h"
#include "game_settings.h"
#include "ini_file.h"
#include "profiler.h"

namespace
{
    constexpr float kGamepadAxisScale = 1.0f / 32767.0f;

    struct TextCursor
    {
        std::string text;
        size_t pos = 0;
    };

    std::string ReadTextFile(const char* path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};

        std::ostringstream stream;
        stream << file.rdbuf();
        return stream.str();
    }

    void SkipWhitespaceAndCommas(TextCursor& cursor)
    {
        while (cursor.pos < cursor.text.size())
        {
            const char ch = cursor.text[cursor.pos];
            if (!std::isspace(static_cast<unsigned char>(ch)) && ch != ',')
                break;
            ++cursor.pos;
        }
    }

    bool Match(TextCursor& cursor, char expected)
    {
        SkipWhitespaceAndCommas(cursor);
        if (cursor.pos >= cursor.text.size() || cursor.text[cursor.pos] != expected)
            return false;

        ++cursor.pos;
        return true;
    }

    bool ReadQuotedString(TextCursor& cursor, std::string& out)
    {
        SkipWhitespaceAndCommas(cursor);
        if (cursor.pos >= cursor.text.size() || cursor.text[cursor.pos] != '"')
            return false;

        ++cursor.pos;
        out.clear();

        while (cursor.pos < cursor.text.size())
        {
            const char ch = cursor.text[cursor.pos++];
            if (ch == '"')
                return true;

            out.push_back(ch);
        }

        return false;
    }

    bool SkipValue(TextCursor& cursor);

    bool SkipObject(TextCursor& cursor)
    {
        if (!Match(cursor, '{'))
            return false;

        int depth = 1;
        bool inString = false;

        while (cursor.pos < cursor.text.size() && depth > 0)
        {
            const char ch = cursor.text[cursor.pos++];

            if (ch == '"')
                inString = !inString;

            if (inString)
                continue;

            if (ch == '{')
                ++depth;
            else if (ch == '}')
                --depth;
        }

        return depth == 0;
    }

    bool SkipArray(TextCursor& cursor)
    {
        if (!Match(cursor, '['))
            return false;

        int depth = 1;
        bool inString = false;

        while (cursor.pos < cursor.text.size() && depth > 0)
        {
            const char ch = cursor.text[cursor.pos++];

            if (ch == '"')
                inString = !inString;

            if (inString)
                continue;

            if (ch == '[')
                ++depth;
            else if (ch == ']')
                --depth;
        }

        return depth == 0;
    }

    bool SkipValue(TextCursor& cursor)
    {
        SkipWhitespaceAndCommas(cursor);
        if (cursor.pos >= cursor.text.size())
            return false;

        if (cursor.text[cursor.pos] == '{')
            return SkipObject(cursor);

        if (cursor.text[cursor.pos] == '[')
            return SkipArray(cursor);

        if (cursor.text[cursor.pos] == '"')
        {
            std::string ignored;
            return ReadQuotedString(cursor, ignored);
        }

        while (cursor.pos < cursor.text.size())
        {
            const char ch = cursor.text[cursor.pos];
            if (ch == ',' || ch == '}' || ch == ']')
                break;
            ++cursor.pos;
        }

        return true;
    }

    bool ReadFieldName(TextCursor& cursor, std::string& name)
    {
        if (!ReadQuotedString(cursor, name))
            return false;

        return Match(cursor, ':');
    }

    bool ReadBoolString(const std::string& value)
    {
        return value == "true" || value == "1";
    }

    float ReadFloatString(const std::string& value)
    {
        return value.empty() ? 0.0f : std::strtof(value.c_str(), nullptr);
    }

    SDL_Scancode KeyboardNameToScancode(const std::string& key)
    {
        auto pos = key.find('_');
        std::string clean = (pos != std::string::npos)
            ? key.substr(pos + 1)
            : key;

        auto res = SDL_GetScancodeFromName(clean.c_str());
        if (res != SDL_SCANCODE_UNKNOWN) return res;

        EngineLogf("Unknown Key %s", clean.c_str());
        return SDL_SCANCODE_UNKNOWN;
    }

    struct GamepadKeyDef
    {
        const char* name;
        InputNativeKeyKind kind;
        SDL_GamepadButton button;
        SDL_GamepadAxis axis;
    };

    constexpr SDL_GamepadButton kNoButton = SDL_GAMEPAD_BUTTON_INVALID;
    constexpr SDL_GamepadAxis kNoAxis = SDL_GAMEPAD_AXIS_INVALID;

    constexpr GamepadKeyDef kGamepadKeys[] = {
        // Thumbsticks
        { "Gamepad_LeftAxis_2D", InputNativeKeyKind::GamepadLeftAxis2D, kNoButton, kNoAxis },
        { "Gamepad_RightAxis_2D", InputNativeKeyKind::GamepadRightAxis2D, kNoButton, kNoAxis },
        { "Gamepad_LeftAxis_X", InputNativeKeyKind::GamepadAxis, kNoButton, SDL_GAMEPAD_AXIS_LEFTX },
        { "Gamepad_LeftAxis_Y", InputNativeKeyKind::GamepadAxis, kNoButton, SDL_GAMEPAD_AXIS_LEFTY },
        { "Gamepad_RightAxis_X", InputNativeKeyKind::GamepadAxis, kNoButton, SDL_GAMEPAD_AXIS_RIGHTX },
        { "Gamepad_RightAxis_Y", InputNativeKeyKind::GamepadAxis, kNoButton, SDL_GAMEPAD_AXIS_RIGHTY },

        // Triggers
        { "Gamepad_LeftTrigger", InputNativeKeyKind::GamepadAxis, kNoButton, SDL_GAMEPAD_AXIS_LEFT_TRIGGER },
        { "Gamepad_RightTrigger", InputNativeKeyKind::GamepadAxis, kNoButton, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER },

        // Face buttons
        { "Gamepad_Face_Top", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_NORTH, kNoAxis },
        { "Gamepad_Face_Left", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_WEST, kNoAxis },
        { "Gamepad_Face_Bottom", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_SOUTH, kNoAxis },
        { "Gamepad_Face_Right", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST, kNoAxis },

        // Special buttons
        { "Gamepad_Special_Left", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_BACK, kNoAxis },
        { "Gamepad_Special_Right", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_START, kNoAxis },

        // Stick buttons
        { "Gamepad_LeftStick", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_LEFT_STICK, kNoAxis },
        { "Gamepad_RightStick", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_RIGHT_STICK, kNoAxis },

        // Bumpers
        { "Gamepad_LeftBumper", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kNoAxis },
        { "Gamepad_RightBumper", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kNoAxis },

        // D-Pad
        { "Gamepad_DPad_Up", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_DPAD_UP, kNoAxis },
        { "Gamepad_DPad_Down", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_DPAD_DOWN, kNoAxis },
        { "Gamepad_DPad_Left", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_DPAD_LEFT, kNoAxis },
        { "Gamepad_DPad_Right", InputNativeKeyKind::GamepadButton, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kNoAxis },
    };

    struct MouseKeyDef
    {
        const char* name;
        InputNativeKeyKind kind;
        uint8_t button;
        const char* displayName;
    };

    constexpr MouseKeyDef kMouseKeys[] = {
        { "Mouse_Left", InputNativeKeyKind::MouseButton, SDL_BUTTON_LEFT, "Left Mouse" },
        { "Mouse_Right", InputNativeKeyKind::MouseButton, SDL_BUTTON_RIGHT, "Right Mouse" },
        { "Mouse_Middle", InputNativeKeyKind::MouseButton, SDL_BUTTON_MIDDLE, "Middle Mouse" },
        { "Mouse_Button4", InputNativeKeyKind::MouseButton, SDL_BUTTON_X1, "Mouse 4" },
        { "Mouse_Button5", InputNativeKeyKind::MouseButton, SDL_BUTTON_X2, "Mouse 5" },
        { "Mouse_WheelUp", InputNativeKeyKind::MouseWheelUp, 0, "Wheel Up" },
        { "Mouse_WheelDown", InputNativeKeyKind::MouseWheelDown, 0, "Wheel Down" },
    };

    bool NativeKeyFromName(const std::string& key, InputBinding& binding)
    {
        if (key.rfind("Keyboard_", 0) == 0)
        {
            binding.keyKind = InputNativeKeyKind::Keyboard;
            binding.keyboardKey = KeyboardNameToScancode(key);
            return binding.keyboardKey != SDL_SCANCODE_UNKNOWN;
        }

        if (key.rfind("Gamepad_", 0) == 0)
        {
            for (const GamepadKeyDef& def : kGamepadKeys)
            {
                if (key != def.name)
                    continue;

                binding.keyKind = def.kind;
                binding.gamepadButton = def.button;
                binding.gamepadAxis = def.axis;
                return true;
            }

            EngineLogf("Unknown gamepad key %s", key.c_str());
            return false;
        }

        if (key == "Mouse_2D_Axis")
        {
            binding.keyKind = InputNativeKeyKind::Mouse2DAxis;
            return true;
        }

        for (const MouseKeyDef& def : kMouseKeys)
        {
            if (key != def.name)
                continue;
            binding.keyKind = def.kind;
            binding.mouseButton = def.button;
            return true;
        }

        EngineLogf("Unknown input key %s", key.c_str());
        return false;
    }

    InputActionValueType ValueTypeFromString(const std::string& value)
    {
        if (value == "boolean")
            return InputActionValueType::Boolean;
        if (value == "vector3")
            return InputActionValueType::Vector3;
        return InputActionValueType::Vector2;
    }

    bool ParseNegateValue(TextCursor& cursor, InputModifier& modifier)
    {
        if (!Match(cursor, '{'))
            return false;

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == '}')
            {
                ++cursor.pos;
                return true;
            }

            std::string field;
            if (!ReadFieldName(cursor, field))
                return false;

            std::string value;
            if (!ReadQuotedString(cursor, value))
                return false;

            if (field == "x")
                modifier.negateX = ReadBoolString(value);
            else if (field == "y")
                modifier.negateY = ReadBoolString(value);
            else if (field == "z")
                modifier.negateZ = ReadBoolString(value);
        }

        return false;
    }

    bool ParseSwizzleString(const std::string& text, int out[3])
    {
        if (text.size() != 3)
            return false;

        bool used[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            const char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
            if (ch < 'X' || ch > 'Z')
                return false;

            const int index = ch - 'X';
            if (used[index])
                return false;

            used[index] = true;
            out[i] = index;
        }

        return true;
    }

    bool ParseModifier(TextCursor& cursor, InputModifier& modifier, bool& recognized)
    {
        if (!Match(cursor, '{'))
            return false;

        std::string modifierName;
        std::string stringValue;
        recognized = false;

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == '}')
            {
                ++cursor.pos;

                if (modifierName == "InputMod_deadzone")
                {
                    modifier.kind = InputModifierKind::Deadzone;
                    recognized = true;
                }
                else if (modifierName == "InputMod_negate")
                {
                    modifier.kind = InputModifierKind::Negate;
                    recognized = true;
                }
                else if (modifierName == "InputMod_swizzle")
                {
                    if (ParseSwizzleString(stringValue, modifier.swizzle))
                    {
                        modifier.kind = InputModifierKind::Swizzle;
                        recognized = true;
                    }
                    else
                    {
                        EngineLogf("InputMod_swizzle needs a value like \"YXZ\", got \"%s\"", stringValue.c_str());
                    }
                }
                else if (modifierName == "InputMod_toWorldSpace")
                {
                    // Removed: movement is rotated into world space by the player.
                    EngineLogf("Input modifier %s is no longer supported and was ignored", modifierName.c_str());
                }
                else if (!modifierName.empty())
                {
                    EngineLogf("Unknown input modifier %s was ignored", modifierName.c_str());
                }

                return !modifierName.empty();
            }

            std::string field;
            if (!ReadFieldName(cursor, field))
                return false;

            if (field == "name")
            {
                if (!ReadQuotedString(cursor, modifierName))
                    return false;
            }
            else if (field == "value")
            {
                SkipWhitespaceAndCommas(cursor);
                if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == '{')
                {
                    if (!ParseNegateValue(cursor, modifier))
                        return false;
                }
                else
                {
                    std::string value;
                    if (!ReadQuotedString(cursor, value))
                        return false;
                    modifier.scalarValue = ReadFloatString(value);
                    stringValue = value;
                }
            }
            else if (!SkipValue(cursor))
            {
                return false;
            }
        }

        return false;
    }

    bool ParseModifiers(TextCursor& cursor, std::vector<InputModifier>& modifiers)
    {
        if (!Match(cursor, '['))
            return false;

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == ']')
            {
                ++cursor.pos;
                return true;
            }

            InputModifier modifier = {};
            bool recognized = false;
            if (!ParseModifier(cursor, modifier, recognized))
                return false;

            if (recognized)
                modifiers.push_back(modifier);
        }

        return false;
    }

    bool ParseBinding(TextCursor& cursor, InputBinding& binding)
    {
        if (!Match(cursor, '{'))
            return false;

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == '}')
            {
                ++cursor.pos;
                return true;
            }

            std::string field;
            if (!ReadFieldName(cursor, field))
                return false;

            if (field == "key")
            {
                std::string key;
                if (!ReadQuotedString(cursor, key))
                    return false;
                if (NativeKeyFromName(key, binding))
                {
                    binding.key = key;
                    binding.defaultKey = key;
                }
            }
            else if (field == "modifiers")
            {
                if (!ParseModifiers(cursor, binding.modifiers))
                    return false;
            }
            else if (field == "bPlayerAssignable")
            {
                std::string value;
                if (!ReadQuotedString(cursor, value))
                    return false;
                binding.playerAssignable = ReadBoolString(value);
            }
            else if (field == "AssignableKey")
            {
                if (!ReadQuotedString(cursor, binding.assignableKey))
                    return false;
            }
            else if (!SkipValue(cursor))
            {
                return false;
            }
        }

        return false;
    }

    // Shape of a binding's value: rebinding keeps it.
    enum class KeyShape
    {
        Button,
        Axis,
        Stick
    };

    KeyShape ShapeOf(InputNativeKeyKind kind)
    {
        switch (kind)
        {
        case InputNativeKeyKind::Mouse2DAxis:
        case InputNativeKeyKind::GamepadLeftAxis2D:
        case InputNativeKeyKind::GamepadRightAxis2D:
            return KeyShape::Stick;
        case InputNativeKeyKind::Mouse_X_Axis:
        case InputNativeKeyKind::Mouse_Y_Axis:
            return KeyShape::Axis;
        default:
            return KeyShape::Button; // gamepad axes too: triggers and half sticks read like buttons
        }
    }

    InputDevice DeviceOf(InputNativeKeyKind kind)
    {
        switch (kind)
        {
        case InputNativeKeyKind::GamepadButton:
        case InputNativeKeyKind::GamepadAxis:
        case InputNativeKeyKind::GamepadLeftAxis2D:
        case InputNativeKeyKind::GamepadRightAxis2D:
            return InputDevice::Gamepad;
        default:
            return InputDevice::KeyboardMouse;
        }
    }

    // Points the binding at another key; false (and unchanged) for an unknown name.
    bool ApplyKeyName(InputBinding& binding, const std::string& keyName)
    {
        InputBinding parsed = {};
        if (!NativeKeyFromName(keyName, parsed))
            return false;
        binding.keyKind = parsed.keyKind;
        binding.keyboardKey = parsed.keyboardKey;
        binding.gamepadButton = parsed.gamepadButton;
        binding.gamepadAxis = parsed.gamepadAxis;
        binding.mouseButton = parsed.mouseButton;
        binding.key = keyName;
        return true;
    }

    bool ParseNativeKeys(TextCursor& cursor, std::vector<InputBinding>& bindings)
    {
        if (!Match(cursor, '['))
            return false;

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == ']')
            {
                ++cursor.pos;
                return true;
            }

            InputBinding binding = {};
            if (!ParseBinding(cursor, binding))
                return false;

            // An unknown key loses this binding, not the whole map.
            if (binding.keyKind != InputNativeKeyKind::Unknown)
            {
                // A key chosen in the map's AssignableKey replaces the default one.
                if (binding.playerAssignable && !binding.assignableKey.empty() && binding.assignableKey != "none")
                    ApplyKeyName(binding, binding.assignableKey);
                bindings.push_back(binding);
            }
        }

        return false;
    }

    // `explicitContext` is set when the action has a "Context" field.
    bool ParseAction(TextCursor& cursor, const std::string& name, InputAction& action, bool& explicitContext)
    {
        if (!Match(cursor, '{'))
            return false;

        action.name = name;
        explicitContext = false;

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos < cursor.text.size() && cursor.text[cursor.pos] == '}')
            {
                ++cursor.pos;
                return true;
            }

            std::string field;
            if (!ReadFieldName(cursor, field))
                return false;

            if (field == "NativeKeys")
            {
                if (!ParseNativeKeys(cursor, action.bindings))
                    return false;
            }
            else if (field == "ValueType")
            {
                std::string valueType;
                if (!ReadQuotedString(cursor, valueType))
                    return false;
                action.valueType = ValueTypeFromString(valueType);
            }
            else if (field == "Context")
            {
                std::string context;
                if (!ReadQuotedString(cursor, context))
                    return false;
                action.context = (context == "UI" || context == "ui") ? InputActionContext::UI : InputActionContext::Game;
                explicitContext = true;
            }
            else if (!SkipValue(cursor))
            {
                return false;
            }
        }

        return false;
    }

    Vector3 Add(const Vector3& a, const Vector3& b)
    {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    float Length2D(float x, float y)
    {
        return std::sqrt(x * x + y * y);
    }

    Vector3 ApplyDeadzone(const Vector3& value, float deadzone)
    {
        const float length = Length2D(value.x, value.y);
        if (length <= deadzone)
            return {};

        return value;
    }

    Vector3 ApplyNegate(const Vector3& value, const InputModifier& modifier)
    {
        return {
            modifier.negateX ? -value.x : value.x,
            modifier.negateY ? -value.y : value.y,
            modifier.negateZ ? -value.z : value.z
        };
    }

    Vector3 ApplySwizzle(const Vector3& value, const InputModifier& modifier)
    {
        const float components[3] = { value.x, value.y, value.z };
        return {
            components[modifier.swizzle[0]],
            components[modifier.swizzle[1]],
            components[modifier.swizzle[2]]
        };
    }

    Vector3 ApplyModifiers(Vector3 value, const std::vector<InputModifier>& modifiers)
    {
        for (const InputModifier& modifier : modifiers)
        {
            switch (modifier.kind)
            {
            case InputModifierKind::Deadzone:
                value = ApplyDeadzone(value, modifier.scalarValue);
                break;
            case InputModifierKind::Negate:
                value = ApplyNegate(value, modifier);
                break;
            case InputModifierKind::Swizzle:
                value = ApplySwizzle(value, modifier);
                break;
            }
        }

        return value;
    }

    Vector3 ReadBindingValue(const InputState& input, const InputBinding& binding, InputActionContext context)
    {
        if (context == InputActionContext::Game ? !input.gameInputEnabled : !input.uiActionsEnabled)
            return {};
        if (context == InputActionContext::UI && input.uiTextInput && binding.keyKind == InputNativeKeyKind::Keyboard)
            return {};

        switch (binding.keyKind)
        {
        case InputNativeKeyKind::Keyboard:
            if (binding.keyboardKey >= 0 && binding.keyboardKey < SDL_SCANCODE_COUNT &&
                (input.keys[binding.keyboardKey] || input.keysTapped[binding.keyboardKey]))
                return { 0.0f, 1.0f, 0.0f };
            return {};

        case InputNativeKeyKind::Mouse2DAxis:
            if (!input.mouseLookEnabled)
                return {};
            return { input.mouseDeltaX, input.mouseDeltaY, 0.0f };

        case InputNativeKeyKind::MouseButton:
            if (binding.mouseButton < 8 && (input.mouseButtons[binding.mouseButton] || input.mouseButtonsTapped[binding.mouseButton]))
                return { 0.0f, 1.0f, 0.0f };
            return {};

        case InputNativeKeyKind::MouseWheelUp:
            return input.mouseWheel > 0.0f ? Vector3{ 0.0f, 1.0f, 0.0f } : Vector3{};

        case InputNativeKeyKind::MouseWheelDown:
            return input.mouseWheel < 0.0f ? Vector3{ 0.0f, 1.0f, 0.0f } : Vector3{};

        case InputNativeKeyKind::GamepadButton:
            if (input.gamepad && SDL_GetGamepadButton(input.gamepad, binding.gamepadButton))
                return { 0.0f, 1.0f, 0.0f };
            return {};

        case InputNativeKeyKind::GamepadAxis:
        {
            if (!input.gamepad)
                return {};

            const float axis = SDL_GetGamepadAxis(input.gamepad, binding.gamepadAxis) * kGamepadAxisScale;

            switch (binding.gamepadAxis)
            {
            case SDL_GAMEPAD_AXIS_LEFTX:
            case SDL_GAMEPAD_AXIS_RIGHTX:
                return { axis, 0.0f, 0.0f };

            // Stick Y axes and triggers go in Y, same as buttons/keys, so they combine with them.
            case SDL_GAMEPAD_AXIS_LEFTY:
            case SDL_GAMEPAD_AXIS_RIGHTY:
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                return { 0.0f, axis, 0.0f };

            default:
                return {};
            }
        }

        case InputNativeKeyKind::GamepadLeftAxis2D:
            if (input.gamepad)
            {
                return {
                    SDL_GetGamepadAxis(input.gamepad, SDL_GAMEPAD_AXIS_LEFTX) * kGamepadAxisScale,
                    SDL_GetGamepadAxis(input.gamepad, SDL_GAMEPAD_AXIS_LEFTY) * kGamepadAxisScale,
                    0.0f
                };
            }
            return {};

        case InputNativeKeyKind::GamepadRightAxis2D:
            if (input.gamepad)
            {
                return {
                    SDL_GetGamepadAxis(input.gamepad, SDL_GAMEPAD_AXIS_RIGHTX) * kGamepadAxisScale,
                    SDL_GetGamepadAxis(input.gamepad, SDL_GAMEPAD_AXIS_RIGHTY) * kGamepadAxisScale,
                    0.0f
                };
            }
            return {};

        default:
            return {};
        }
    }

    bool IsNonZero(const Vector3& value)
    {
        return std::fabs(value.x) > 0.0001f || std::fabs(value.y) > 0.0001f || std::fabs(value.z) > 0.0001f;
    }
}

namespace
{
    constexpr Sint16 kGamepadActivateAxis = 16000; // about half tilt

    void OpenGamepad(InputState& input, SDL_JoystickID id)
    {
        for (SDL_Gamepad* gamepad : input.gamepads)
        {
            if (SDL_GetGamepadID(gamepad) == id)
                return;
        }
        SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
        if (!gamepad)
            return;
        input.gamepads.push_back(gamepad);
        if (!input.gamepad)
            input.gamepad = gamepad;
        const char* name = SDL_GetGamepadName(gamepad);
        EngineLogf("Gamepad connected: %s", name ? name : "unknown");
    }

    void MakeGamepadActive(InputState& input, SDL_JoystickID id)
    {
        if (input.gamepad && SDL_GetGamepadID(input.gamepad) == id)
            return;
        for (SDL_Gamepad* gamepad : input.gamepads)
        {
            if (SDL_GetGamepadID(gamepad) == id)
            {
                input.gamepad = gamepad;
                return;
            }
        }
    }

    const char* GamepadButtonKeyName(SDL_GamepadButton button)
    {
        for (const GamepadKeyDef& def : kGamepadKeys)
        {
            if (def.kind == InputNativeKeyKind::GamepadButton && def.button == button)
                return def.name;
        }
        return nullptr;
    }

    const char* GamepadAxisKeyName(SDL_GamepadAxis axis)
    {
        for (const GamepadKeyDef& def : kGamepadKeys)
        {
            if (def.kind == InputNativeKeyKind::GamepadAxis && def.axis == axis)
                return def.name;
        }
        return nullptr;
    }

    // Takes the event when it is the press InputBeginKeyCapture waits for.
    bool CaptureKey(InputState& input, const SDL_Event& e)
    {
        std::string key;
        const bool keyboardMouse = input.captureDevice == InputDevice::KeyboardMouse;
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
            if (keyboardMouse && !e.key.repeat)
            {
                const char* name = SDL_GetScancodeName(e.key.scancode);
                if (name && name[0])
                    key = std::string("Keyboard_") + name;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            for (const MouseKeyDef& def : kMouseKeys)
            {
                if (keyboardMouse && def.kind == InputNativeKeyKind::MouseButton && def.button == e.button.button)
                    key = def.name;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (keyboardMouse && e.wheel.y != 0.0f)
                key = (e.wheel.y > 0.0f) == (e.wheel.direction != SDL_MOUSEWHEEL_FLIPPED) ? "Mouse_WheelUp" : "Mouse_WheelDown";
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (!keyboardMouse)
            {
                const char* name = GamepadButtonKeyName(static_cast<SDL_GamepadButton>(e.gbutton.button));
                key = name ? name : "";
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (!keyboardMouse && e.gaxis.value > kGamepadActivateAxis &&
                (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
            {
                const char* name = GamepadAxisKeyName(static_cast<SDL_GamepadAxis>(e.gaxis.axis));
                key = name ? name : "";
            }
            break;
        default:
            break;
        }
        if (key.empty())
            return false;
        input.capturedKey = key;
        input.captureActive = false;
        return true;
    }
}

void InputInit(InputState& input)
{
    KES_ZONE_FN(INPUT);
    input = {};

    int gamepadCount = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&gamepadCount))
    {
        for (int i = 0; i < gamepadCount; ++i)
            OpenGamepad(input, ids[i]);
        SDL_free(ids);
    }

    // config/ ships next to the executable (copied by the build and packaging).
    // Fall back to the working directory for older setups.
    std::string error;
    const std::filesystem::path exeConfig = GetExecutableDirectory() / "config" / "default.imap";
    if (!InputLoadActionMap(input, exeConfig.string().c_str(), &error) &&
        !InputLoadActionMap(input, "config/default.imap", &error))
    {
        EngineLogf("Input map not loaded (%s): %s", exeConfig.string().c_str(), error.c_str());
    }
}

void InputShutdown(InputState& input)
{
    for (SDL_Gamepad* gamepad : input.gamepads)
        SDL_CloseGamepad(gamepad);
    input.gamepads.clear();
    input.gamepad = nullptr;
}

void InputBeginFrame(InputState& input)
{
    KES_ZONE_FN(INPUT);
    for (int key = 0; key < SDL_SCANCODE_COUNT; ++key)
        input.previousKeys[key] = input.keys[key] || input.keysTapped[key];
    std::fill(std::begin(input.keysTapped), std::end(input.keysTapped), false);
    std::copy(std::begin(input.mouseButtons), std::end(input.mouseButtons), std::begin(input.previousMouseButtons));
    std::fill(std::begin(input.mouseButtonsTapped), std::end(input.mouseButtonsTapped), false);

    input.mouseDeltaX = 0.0f;
    input.mouseDeltaY = 0.0f;
    input.mouseWheel = 0.0f;

    for (auto& pair : input.actionStates)
    {
        pair.second.pressed = false;
        pair.second.released = false;
        pair.second.value2 = {};
        pair.second.value3 = {};
    }
}

bool InputCaptureEvent(InputState& input, const SDL_Event& e)
{
    return input.captureActive && CaptureKey(input, e);
}

void InputProcessEvent(InputState& input, const SDL_Event& e)
{
    if (InputCaptureEvent(input, e))
        return;

    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat && e.key.scancode >= 0 && e.key.scancode < SDL_SCANCODE_COUNT)
        {
            input.keys[e.key.scancode] = true;
            input.keysTapped[e.key.scancode] = true;
        }
        input.activeDevice = InputDevice::KeyboardMouse;
        break;

    case SDL_EVENT_KEY_UP:
        if (e.key.scancode >= 0 && e.key.scancode < SDL_SCANCODE_COUNT)
            input.keys[e.key.scancode] = false;
        break;

    case SDL_EVENT_MOUSE_MOTION:
        input.mouseDeltaX += static_cast<float>(e.motion.xrel);
        input.mouseDeltaY += static_cast<float>(e.motion.yrel);
        if (std::fabs(e.motion.xrel) + std::fabs(e.motion.yrel) > 2.0f) // not sensor jitter
            input.activeDevice = InputDevice::KeyboardMouse;
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button < 8)
        {
            input.mouseButtons[e.button.button] = true;
            input.mouseButtonsTapped[e.button.button] = true;
        }
        input.activeDevice = InputDevice::KeyboardMouse;
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        input.mouseWheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        input.activeDevice = InputDevice::KeyboardMouse;
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button < 8)
            input.mouseButtons[e.button.button] = false;
        break;

    case SDL_EVENT_GAMEPAD_ADDED:
        OpenGamepad(input, e.gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        for (auto it = input.gamepads.begin(); it != input.gamepads.end(); ++it)
        {
            if (SDL_GetGamepadID(*it) != e.gdevice.which)
                continue;
            if (input.gamepad == *it)
                input.gamepad = nullptr;
            SDL_CloseGamepad(*it);
            input.gamepads.erase(it);
            break;
        }
        if (!input.gamepad && !input.gamepads.empty())
            input.gamepad = input.gamepads.front();
        break;

    // The controller used last becomes the active one.
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        MakeGamepadActive(input, e.gbutton.which);
        input.activeDevice = InputDevice::Gamepad;
        break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.value > kGamepadActivateAxis || e.gaxis.value < -kGamepadActivateAxis)
        {
            MakeGamepadActive(input, e.gaxis.which);
            input.activeDevice = InputDevice::Gamepad;
        }
        break;
    }
}

void InputEndFrame(InputState& input)
{
    KES_ZONE_FN(INPUT);
    InputEvaluateActions(input);
}

void InputResetState(InputState& input)
{
    KES_ZONE_FN(INPUT);
    std::fill(std::begin(input.keys), std::end(input.keys), false);
    std::fill(std::begin(input.keysTapped), std::end(input.keysTapped), false);
    std::fill(std::begin(input.mouseButtons), std::end(input.mouseButtons), false);
    std::fill(std::begin(input.mouseButtonsTapped), std::end(input.mouseButtonsTapped), false);
    input.mouseDeltaX = 0.0f;
    input.mouseDeltaY = 0.0f;
    input.mouseWheel = 0.0f;
}

namespace
{
    bool ParseActionMap(std::string_view text, std::vector<InputAction>& outActions, std::string* outError)
    {
        TextCursor cursor = {};
        cursor.text = std::string(text);

        while (cursor.pos < cursor.text.size())
        {
            SkipWhitespaceAndCommas(cursor);
            if (cursor.pos >= cursor.text.size())
                break;

            std::string actionName;
            if (!ReadFieldName(cursor, actionName))
            {
                if (outError)
                    *outError = "Failed to read input action name.";
                return false;
            }

            InputAction action = {};
            bool explicitContext = false;
            if (!ParseAction(cursor, actionName, action, explicitContext))
            {
                if (outError)
                    *outError = "Failed to parse input action: " + actionName;
                return false;
            }
            // Actions named UI_* are UI actions unless "Context" says otherwise.
            if (!explicitContext && action.name.rfind("UI_", 0) == 0)
                action.context = InputActionContext::UI;

            outActions.push_back(std::move(action));
        }

        return true;
    }
}

bool InputLoadActionMap(InputState& input, const char* path, std::string* outError)
{
    KES_ZONE_FN(INPUT);
    KES_ZONE_TEXTF(INPUT, "%s", path ? path : "<null>");
    const std::string text = ReadTextFile(path);
    if (text.empty())
    {
        if (outError)
            *outError = "Could not read input map.";
        return false;
    }

    std::vector<InputAction> actions;
    if (!ParseActionMap(text, actions, outError))
        return false;

    input.actions = std::move(actions);
    input.actionStates.clear();
    for (const InputAction& action : input.actions)
        input.actionStates[action.name] = {};
    return true;
}

bool InputAddDefaultActions(InputState& input, std::string_view mapText, std::string* outError)
{
    std::vector<InputAction> actions;
    if (!ParseActionMap(mapText, actions, outError))
        return false;
    for (InputAction& action : actions)
    {
        if (input.actionStates.find(std::string_view(action.name)) != input.actionStates.end())
            continue; // the project's map defines it
        input.actionStates[action.name] = {};
        input.actions.push_back(std::move(action));
    }
    return true;
}

bool InputIsKeyBound(const InputState& input, SDL_Scancode key, InputActionContext context)
{
    for (const InputAction& action : input.actions)
    {
        if (action.context != context)
            continue;
        for (const InputBinding& binding : action.bindings)
        {
            if (binding.keyKind == InputNativeKeyKind::Keyboard && binding.keyboardKey == key)
                return true;
        }
    }
    return false;
}

void InputEvaluateActions(InputState& input)
{
    KES_ZONE_FN(INPUT);
    // Keys held while game input was off (e.g. the Escape that closed a menu)
    // read as down when it comes back on, but not as a fresh press.
    const bool gameJustEnabled = input.gameInputEnabled && !input.gameInputWasEnabled;
    const bool uiJustEnabled = input.uiActionsEnabled && !input.uiActionsWasEnabled;
    input.gameInputWasEnabled = input.gameInputEnabled;
    input.uiActionsWasEnabled = input.uiActionsEnabled;
    for (const InputAction& action : input.actions)
    {
        const bool justEnabled = action.context == InputActionContext::Game ? gameJustEnabled : uiJustEnabled;
        InputActionState& state = input.actionStates[action.name];
        const bool wasDown = state.down;

        Vector3 accumulated = {};

        for (const InputBinding& binding : action.bindings)
        {
            Vector3 value = ReadBindingValue(input, binding, action.context);
            value = ApplyModifiers(value, binding.modifiers);
            accumulated = Add(accumulated, value);
        }

        state.value3 = accumulated;
        state.value2 = { accumulated.x, accumulated.y };

        state.down = IsNonZero(accumulated);
        state.pressed = state.down && !wasDown && !justEnabled;
        state.released = !state.down && wasDown;
    }
}

bool InputActionDown(const InputState& input, const char* actionName)
{
    const auto found = input.actionStates.find(std::string_view(actionName ? actionName : ""));
    return found != input.actionStates.end() && found->second.down;
}

bool InputActionPressed(const InputState& input, const char* actionName)
{
    const auto found = input.actionStates.find(std::string_view(actionName ? actionName : ""));
    return found != input.actionStates.end() && found->second.pressed;
}

bool InputActionReleased(const InputState& input, const char* actionName)
{
    const auto found = input.actionStates.find(std::string_view(actionName ? actionName : ""));
    return found != input.actionStates.end() && found->second.released;
}

Vector2 InputActionValue2D(const InputState& input, const char* actionName)
{
    const auto found = input.actionStates.find(std::string_view(actionName ? actionName : ""));
    return found != input.actionStates.end() ? found->second.value2 : Vector2{};
}

Vector3 InputActionValue3D(const InputState& input, const char* actionName)
{
    const auto found = input.actionStates.find(std::string_view(actionName ? actionName : ""));
    return found != input.actionStates.end() ? found->second.value3 : Vector3{};
}

bool InputKeyDown(const InputState& input, SDL_Scancode key)
{
    return key >= 0 && key < SDL_SCANCODE_COUNT && (input.keys[key] || input.keysTapped[key]);
}

bool InputKeyPressed(const InputState& input, SDL_Scancode key)
{
    return key >= 0 && key < SDL_SCANCODE_COUNT && (input.keys[key] || input.keysTapped[key]) && !input.previousKeys[key];
}

bool InputKeyReleased(const InputState& input, SDL_Scancode key)
{
    return key >= 0 && key < SDL_SCANCODE_COUNT && !input.keys[key] && !input.keysTapped[key] && input.previousKeys[key];
}
InputDevice InputGetActiveDevice(const InputState& input)
{
    return input.activeDevice;
}

SDL_GamepadType InputGetGamepadType(const InputState& input)
{
    return input.gamepad ? SDL_GetGamepadType(input.gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

namespace
{
    InputAction* FindAction(InputState& input, const char* actionName)
    {
        for (InputAction& action : input.actions)
        {
            if (actionName && action.name == actionName)
                return &action;
        }
        return nullptr;
    }

    const InputAction* FindAction(const InputState& input, const char* actionName)
    {
        return FindAction(const_cast<InputState&>(input), actionName);
    }

    constexpr const char* kOverrideSection = "InputBindings";

    std::string OverridePath(const char* path)
    {
        return path && path[0] ? std::string(path) : (GetUserDataDirectory() / "Config" / "InputBindings.ini").string();
    }
}

std::vector<InputBindingInfo> InputGetBindings(const InputState& input, const char* actionName)
{
    std::vector<InputBindingInfo> result;
    if (const InputAction* action = FindAction(input, actionName))
    {
        for (size_t index = 0; index < action->bindings.size(); ++index)
        {
            const InputBinding& binding = action->bindings[index];
            result.push_back({ index, binding.key, binding.defaultKey, DeviceOf(binding.keyKind), binding.playerAssignable });
        }
    }
    return result;
}

bool InputRebind(InputState& input, const char* actionName, size_t bindingIndex, const char* keyName, std::string* outError)
{
    const auto fail = [&](std::string message)
    {
        if (outError)
            *outError = std::move(message);
        return false;
    };
    InputAction* action = FindAction(input, actionName);
    if (!action)
        return fail(std::string("No input action ") + (actionName ? actionName : "<null>"));
    if (bindingIndex >= action->bindings.size())
        return fail("No such binding");
    InputBinding& binding = action->bindings[bindingIndex];
    if (!binding.playerAssignable)
        return fail("The binding is not player assignable");

    InputBinding parsed = {};
    const std::string key = keyName ? keyName : "";
    if (!NativeKeyFromName(key, parsed))
        return fail("Unknown key " + key);
    if (DeviceOf(parsed.keyKind) != DeviceOf(binding.keyKind) || ShapeOf(parsed.keyKind) != ShapeOf(binding.keyKind))
        return fail("A " + binding.defaultKey + " binding can't use " + key);

    ApplyKeyName(binding, key);
    return true;
}

void InputResetBinding(InputState& input, const char* actionName, size_t bindingIndex)
{
    InputAction* action = FindAction(input, actionName);
    if (action && bindingIndex < action->bindings.size())
        ApplyKeyName(action->bindings[bindingIndex], action->bindings[bindingIndex].defaultKey);
}

void InputResetAllBindings(InputState& input)
{
    for (InputAction& action : input.actions)
    {
        for (InputBinding& binding : action.bindings)
        {
            if (binding.key != binding.defaultKey)
                ApplyKeyName(binding, binding.defaultKey);
        }
    }
}

std::vector<std::string> InputFindActionsUsingKey(const InputState& input, const char* keyName, InputActionContext context)
{
    std::vector<std::string> result;
    for (const InputAction& action : input.actions)
    {
        if (action.context != context)
            continue;
        const bool uses = std::any_of(action.bindings.begin(), action.bindings.end(),
            [&](const InputBinding& binding)
            { return keyName && binding.key == keyName; });
        if (uses)
            result.push_back(action.name);
    }
    return result;
}

bool InputSaveBindingOverrides(const InputState& input, const char* path, std::string* outError)
{
    IniFile ini;
    for (const InputAction& action : input.actions)
    {
        for (const InputBinding& binding : action.bindings)
        {
            if (binding.playerAssignable && binding.key != binding.defaultKey)
                ini.SetString(kOverrideSection, action.name + "|" + binding.defaultKey, binding.key);
        }
    }
    const std::string file = OverridePath(path);
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), error);
    if (!ini.SaveToFile(file))
    {
        if (outError)
            *outError = "Could not write " + file;
        return false;
    }
    return true;
}

bool InputLoadBindingOverrides(InputState& input, const char* path, std::string* outError)
{
    const std::string file = OverridePath(path);
    std::error_code existsError;
    if (!std::filesystem::exists(file, existsError))
        return true; // nothing rebound yet
    IniFile ini;
    if (!ini.LoadFromFile(file))
    {
        if (outError)
            *outError = "Could not read " + file;
        return false;
    }
    for (InputAction& action : input.actions)
    {
        for (size_t index = 0; index < action.bindings.size(); ++index)
        {
            const InputBinding& binding = action.bindings[index];
            const std::string* key = ini.Find(kOverrideSection, action.name + "|" + binding.defaultKey);
            if (!key)
                continue;
            std::string error;
            if (!InputRebind(input, action.name.c_str(), index, key->c_str(), &error))
                EngineLogf("Input binding override %s -> %s ignored: %s", action.name.c_str(), key->c_str(), error.c_str());
        }
    }
    return true;
}

void InputBeginKeyCapture(InputState& input, InputDevice device)
{
    input.captureActive = true;
    input.captureDevice = device;
    input.capturedKey.clear();
}

void InputCancelKeyCapture(InputState& input)
{
    input.captureActive = false;
    input.capturedKey.clear();
}

bool InputIsCapturingKey(const InputState& input)
{
    return input.captureActive;
}

bool InputGetCapturedKey(InputState& input, std::string& outKeyName)
{
    if (input.capturedKey.empty())
        return false;
    outKeyName = std::move(input.capturedKey);
    input.capturedKey.clear();
    return true;
}

std::string InputGetKeyDisplayName(const InputState& input, const std::string& keyName)
{
    InputBinding binding = {};
    if (!NativeKeyFromName(keyName, binding))
        return keyName;

    switch (binding.keyKind)
    {
    case InputNativeKeyKind::Keyboard:
    {
        // The key at this position on the player's layout (Z on AZERTY for W).
        const SDL_Keycode keycode = SDL_GetKeyFromScancode(binding.keyboardKey, SDL_KMOD_NONE, false);
        const char* name = SDL_GetKeyName(keycode);
        return name && name[0] ? std::string(name) : std::string(SDL_GetScancodeName(binding.keyboardKey));
    }
    case InputNativeKeyKind::MouseButton:
    case InputNativeKeyKind::MouseWheelUp:
    case InputNativeKeyKind::MouseWheelDown:
        for (const MouseKeyDef& def : kMouseKeys)
        {
            if (keyName == def.name)
                return def.displayName;
        }
        return keyName;
    case InputNativeKeyKind::Mouse2DAxis:
        return "Mouse";
    default:
        break;
    }

    const SDL_GamepadType type = InputGetGamepadType(input);
    const bool playstation = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    const bool nintendo = type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR ||
        type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT || type == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
    const auto pick = [&](const char* xbox, const char* ps, const char* switchName) -> std::string
    {
        return playstation ? ps : nintendo ? switchName
                                           : xbox;
    };

    if (binding.keyKind == InputNativeKeyKind::GamepadButton)
    {
        switch (SDL_GetGamepadButtonLabelForType(type == SDL_GAMEPAD_TYPE_UNKNOWN ? SDL_GAMEPAD_TYPE_XBOXONE : type, binding.gamepadButton))
        {
        case SDL_GAMEPAD_BUTTON_LABEL_A:        return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B:        return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X:        return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y:        return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:    return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:   return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:   return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default:                                break;
        }
        switch (binding.gamepadButton)
        {
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return pick("LB", "L1", "L");
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return pick("RB", "R1", "R");
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return pick("LS", "L3", "Left Stick");
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return pick("RS", "R3", "Right Stick");
        case SDL_GAMEPAD_BUTTON_BACK:           return pick("View", "Create", "-");
        case SDL_GAMEPAD_BUTTON_START:          return pick("Menu", "Options", "+");
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return "D-Pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return "D-Pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return "D-Pad Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return "D-Pad Right";
        default:                                return keyName;
        }
    }
    if (binding.keyKind == InputNativeKeyKind::GamepadAxis)
    {
        switch (binding.gamepadAxis)
        {
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return pick("LT", "L2", "ZL");
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return pick("RT", "R2", "ZR");
        case SDL_GAMEPAD_AXIS_LEFTX:
        case SDL_GAMEPAD_AXIS_LEFTY:         return "Left Stick";
        default:                             return "Right Stick";
        }
    }
    return binding.keyKind == InputNativeKeyKind::GamepadLeftAxis2D ? "Left Stick" : "Right Stick";
}

std::string InputGetActionDisplayName(const InputState& input, const char* actionName, InputDevice device)
{
    if (const InputAction* action = FindAction(input, actionName))
    {
        for (const InputBinding& binding : action->bindings)
        {
            if (DeviceOf(binding.keyKind) == device)
                return InputGetKeyDisplayName(input, binding.key);
        }
    }
    return {};
}

namespace
{
    Uint16 RumbleStrength(float value)
    {
        return static_cast<Uint16>(std::clamp(value, 0.0f, 1.0f) * 65535.0f);
    }

    bool RumbleAllowed(const InputState& input)
    {
        const GameSettings* settings = GetGameSettings();
        return input.gamepad && (!settings || settings->IsGamepadVibrationEnabled());
    }
}

bool InputRumble(InputState& input, float lowFrequency, float highFrequency, float seconds)
{
    if (!RumbleAllowed(input))
        return false;
    const Uint32 ms = static_cast<Uint32>((std::max)(seconds, 0.0f) * 1000.0f);
    return SDL_RumbleGamepad(input.gamepad, RumbleStrength(lowFrequency), RumbleStrength(highFrequency), ms);
}

bool InputRumbleTriggers(InputState& input, float left, float right, float seconds)
{
    if (!RumbleAllowed(input))
        return false;
    const Uint32 ms = static_cast<Uint32>((std::max)(seconds, 0.0f) * 1000.0f);
    return SDL_RumbleGamepadTriggers(input.gamepad, RumbleStrength(left), RumbleStrength(right), ms);
}

void InputStopRumble(InputState& input)
{
    for (SDL_Gamepad* gamepad : input.gamepads)
    {
        SDL_RumbleGamepad(gamepad, 0, 0, 0);
        SDL_RumbleGamepadTriggers(gamepad, 0, 0, 0);
    }
}

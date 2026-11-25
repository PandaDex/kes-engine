#include "test_framework.h"
#include "input.h"

namespace
{
    SDL_Event KeyEvent(SDL_EventType type, SDL_Scancode scancode)
    {
        SDL_Event event = {};
        event.type = type;
        event.key.scancode = scancode;
        return event;
    }
}

TEST_CASE("input: an unknown key loses its binding, not the map")
{
    InputState input;
    std::string error;
    REQUIRE(InputAddDefaultActions(input,
        "\"Fire\": { \"NativeKeys\": [ {\"key\": \"Gamepad_Nonsense\"}, {\"key\": \"Mouse_Nonsense\"}, {\"key\": \"Keyboard_F\"} ], \"ValueType\": \"boolean\" }\n"
        "\"Jump\": { \"NativeKeys\": [ {\"key\": \"Keyboard_Space\"} ], \"ValueType\": \"boolean\" }\n",
        &error));
    REQUIRE(input.actions.size() == 2);
    CHECK(input.actions[0].bindings.size() == 1);
}

TEST_CASE("input: a press and release within one frame still counts")
{
    InputState input;
    std::string error;
    REQUIRE(InputAddDefaultActions(input, "\"Jump\": { \"NativeKeys\": [ {\"key\": \"Keyboard_Space\"} ], \"ValueType\": \"boolean\" }\n", &error));

    InputBeginFrame(input);
    InputProcessEvent(input, KeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE));
    InputProcessEvent(input, KeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE));
    InputEndFrame(input);
    CHECK(InputActionPressed(input, "Jump"));
    CHECK(InputKeyPressed(input, SDL_SCANCODE_SPACE));

    InputBeginFrame(input);
    InputEndFrame(input);
    CHECK(!InputActionDown(input, "Jump"));
    CHECK(InputActionReleased(input, "Jump"));
    CHECK(InputKeyReleased(input, SDL_SCANCODE_SPACE));

    InputBeginFrame(input);
    InputEndFrame(input);
    CHECK(!InputActionReleased(input, "Jump"));
    CHECK(!InputKeyReleased(input, SDL_SCANCODE_SPACE));
}

namespace
{
    constexpr const char* kRebindMap =
        "\"Jump\": { \"NativeKeys\": [ {\"key\": \"Keyboard_Space\", \"bPlayerAssignable\": \"true\"},"
        " {\"key\": \"Gamepad_Face_Bottom\", \"bPlayerAssignable\": \"true\"} ], \"ValueType\": \"boolean\" }\n"
        "\"Fire\": { \"NativeKeys\": [ {\"key\": \"Mouse_Left\", \"bPlayerAssignable\": \"true\"} ], \"ValueType\": \"boolean\" }\n"
        "\"Next\": { \"NativeKeys\": [ {\"key\": \"Mouse_WheelUp\"} ], \"ValueType\": \"boolean\" }\n"
        "\"Move\": { \"NativeKeys\": [ {\"key\": \"Gamepad_LeftAxis_2D\", \"bPlayerAssignable\": \"true\"},"
        " {\"key\": \"Keyboard_W\"} ], \"ValueType\": \"vector2\" }\n";

    void Frame(InputState& input, std::initializer_list<SDL_Event> events)
    {
        InputBeginFrame(input);
        for (const SDL_Event& event : events)
            InputProcessEvent(input, event);
        InputEndFrame(input);
    }

    SDL_Event MouseButtonEvent(SDL_EventType type, uint8_t button)
    {
        SDL_Event event = {};
        event.type = type;
        event.button.button = button;
        return event;
    }

    SDL_Event WheelEvent(float y)
    {
        SDL_Event event = {};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.y = y;
        event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        return event;
    }
}

TEST_CASE("input: mouse buttons and the wheel drive actions")
{
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));

    Frame(input, { MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT) });
    CHECK(InputActionPressed(input, "Fire"));
    Frame(input, { MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT) });
    CHECK(InputActionReleased(input, "Fire"));

    Frame(input, { WheelEvent(1.0f) });
    CHECK(InputActionPressed(input, "Next"));
    Frame(input, {});
    CHECK(!InputActionDown(input, "Next"));
    Frame(input, { WheelEvent(-1.0f) });
    CHECK(!InputActionDown(input, "Next"));
}

TEST_CASE("input: a rebound action follows its new key")
{
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));
    std::string error;
    REQUIRE(InputRebind(input, "Jump", 0, "Keyboard_F", &error));

    Frame(input, { KeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE) });
    CHECK(!InputActionDown(input, "Jump"));
    Frame(input, { KeyEvent(SDL_EVENT_KEY_UP, SDL_SCANCODE_SPACE), KeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_F) });
    CHECK(InputActionPressed(input, "Jump"));

    // Keyboard and mouse share a device; the modifiers of a binding stay.
    CHECK(InputRebind(input, "Fire", 0, "Keyboard_G", &error));
    CHECK(InputFindActionsUsingKey(input, "Keyboard_F") == std::vector<std::string>{ "Jump" });

    const std::vector<InputBindingInfo> bindings = InputGetBindings(input, "Jump");
    REQUIRE(bindings.size() == 2);
    CHECK(bindings[0].key == "Keyboard_F");
    CHECK(bindings[0].defaultKey == "Keyboard_Space");
    CHECK(bindings[1].device == InputDevice::Gamepad);

    InputResetAllBindings(input);
    CHECK(InputGetBindings(input, "Jump")[0].key == "Keyboard_Space");
}

TEST_CASE("input: keyboard key names have no spaces")
{
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));
    const std::pair<const char*, const char*> names[] = {
        { "Keyboard_Keypad Enter", "Keyboard_KeypadEnter" },
        { "Keyboard_left ctrl", "Keyboard_LeftCtrl" },
        { "Keyboard_-", "Keyboard_Minus" },
        { "Keyboard_Minus", "Keyboard_Minus" },
        { "Keyboard_NumLock", "Keyboard_NumLock" },
        { "Keyboard_F13", "Keyboard_F13" },
        { "Keyboard_7", "Keyboard_7" },
    };
    for (const auto& [given, stored] : names)
    {
        REQUIRE(InputRebind(input, "Jump", 0, given));
        CHECK(InputGetBindings(input, "Jump")[0].key == stored);
    }

    InputBeginKeyCapture(input, InputDevice::KeyboardMouse);
    Frame(input, { KeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RSHIFT) });
    std::string captured;
    REQUIRE(InputGetCapturedKey(input, captured));
    CHECK(captured == "Keyboard_RightShift");
}

TEST_CASE("input: rebinding refuses keys that don't fit")
{
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));
    std::string error;
    CHECK(!InputRebind(input, "Jump", 0, "Gamepad_Face_Left", &error));     // other device
    CHECK(!InputRebind(input, "Jump", 1, "Keyboard_F", &error));
    CHECK(!InputRebind(input, "Move", 0, "Gamepad_Face_Left", &error));     // stick -> button
    CHECK(InputRebind(input, "Move", 0, "Gamepad_RightAxis_2D", &error));   // stick -> stick
    CHECK(!InputRebind(input, "Move", 1, "Keyboard_Up", &error));           // not assignable
    CHECK(!InputRebind(input, "Jump", 0, "Keyboard_NoSuchKey", &error));
    CHECK(!InputRebind(input, "Missing", 0, "Keyboard_F", &error));
    CHECK(InputGetBindings(input, "Jump")[0].key == "Keyboard_Space");
}

TEST_CASE("input: player bindings are saved and loaded")
{
    kestest::TempDir dir;
    const std::string path = dir.File("Config/InputBindings.ini");
    {
        InputState input;
        REQUIRE(InputAddDefaultActions(input, kRebindMap));
        REQUIRE(InputRebind(input, "Jump", 0, "Keyboard_Left Shift"));
        REQUIRE(InputRebind(input, "Jump", 1, "Gamepad_RightTrigger"));
        std::string error;
        REQUIRE(InputSaveBindingOverrides(input, path.c_str(), &error));
    }
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));
    REQUIRE(InputLoadBindingOverrides(input, path.c_str()));
    const std::vector<InputBindingInfo> bindings = InputGetBindings(input, "Jump");
    CHECK(bindings[0].key == "Keyboard_LeftShift"); // old spaced names are stored canonical
    CHECK(bindings[1].key == "Gamepad_RightTrigger");
    CHECK(InputGetBindings(input, "Fire")[0].key == "Mouse_Left");

    InputState missing;
    REQUIRE(InputAddDefaultActions(missing, kRebindMap));
    CHECK(InputLoadBindingOverrides(missing, dir.File("none.ini").c_str()));
}

TEST_CASE("input: key capture takes the next press away from actions")
{
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));
    InputBeginKeyCapture(input, InputDevice::KeyboardMouse);
    CHECK(InputIsCapturingKey(input));

    Frame(input, { KeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE) });
    CHECK(!InputActionDown(input, "Jump"));
    CHECK(!InputIsCapturingKey(input));
    std::string key;
    REQUIRE(InputGetCapturedKey(input, key));
    CHECK(key == "Keyboard_Space");
    CHECK(!InputGetCapturedKey(input, key));

    InputBeginKeyCapture(input, InputDevice::KeyboardMouse);
    Frame(input, { MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT) });
    REQUIRE(InputGetCapturedKey(input, key));
    CHECK(key == "Mouse_Right");
    CHECK(InputRebind(input, "Fire", 0, key.c_str()));
}

TEST_CASE("input: prompts name keys and follow the last device")
{
    InputState input;
    REQUIRE(InputAddDefaultActions(input, kRebindMap));
    CHECK(InputGetKeyDisplayName(input, "Keyboard_Space") == "Space");
    CHECK(InputGetKeyDisplayName(input, "Mouse_Right") == "Right Mouse");
    CHECK(InputGetKeyDisplayName(input, "Gamepad_Face_Bottom") == "A");
    CHECK(InputGetKeyDisplayName(input, "Gamepad_LeftBumper") == "LB");
    CHECK(InputGetActionDisplayName(input, "Jump", InputDevice::Gamepad) == "A");
    CHECK(InputGetActionDisplayName(input, "Fire", InputDevice::Gamepad).empty());

    CHECK(InputGetActiveDevice(input) == InputDevice::KeyboardMouse);
    SDL_Event pad = {};
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    pad.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    Frame(input, { pad });
    CHECK(InputGetActiveDevice(input) == InputDevice::Gamepad);
    Frame(input, { KeyEvent(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A) });
    CHECK(InputGetActiveDevice(input) == InputDevice::KeyboardMouse);
    CHECK(!InputRumble(input, 1.0f, 1.0f, 0.1f));   // no gamepad
}

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

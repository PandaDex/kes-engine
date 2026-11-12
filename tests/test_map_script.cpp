#include "test_framework.h"
#include "entity_manager.h"
#include "game_context.h"
#include "map_script.h"

namespace
{
    struct Received
    {
        std::string input;
        std::string value;
        Entity* caller = nullptr;
        Entity* activator = nullptr;
    };

    class Lamp : public Entity, public IMapScriptable
    {
    public:
        std::vector<Received> received;

        const MapScriptPorts& GetMapScriptPorts() const override
        {
            static const MapScriptPorts ports = { { { "TurnOn" }, { "SetLevel", "float" } }, { { "OnLit" } } };
            return ports;
        }

        bool AcceptInput(const std::string& input, const MapScriptCall& call) override
        {
            if (input != "TurnOn" && input != "SetLevel")
                return false;
            received.push_back({ input, call.value, call.caller, call.activator });
            if (input == "TurnOn")
                FireOutput("OnLit", call.activator, "lit");
            return true;
        }

        void Fire(const char* output, Entity* activator = nullptr, const char* value = "")
        {
            FireOutput(output, activator, value);
        }
    };

    class Plain : public Entity
    {
    };

    struct Fixture
    {
        GameContext context;
        EntityManager entities;
        MapScriptSystem script;

        Fixture()
        {
            context.entities = &entities;
            context.mapScript = &script;
            entities.SetGameContext(&context);
            script.SetGameContext(&context);
        }

        void Link(Entity& from, const char* output, Entity& to, const char* input, const char* parameter = "",
                  float delay = 0.0f, bool once = false)
        {
            MapScriptLink link;
            link.output = output;
            link.target = to.GetGuid();
            link.input = input;
            link.parameter = parameter;
            link.delay = delay;
            link.once = once;
            script.AddLink(from.GetGuid(), link);
        }
    };
}

TEST_CASE("guid: round trips through text and bytes")
{
    Guid parsed;
    REQUIRE(Guid::Parse("{0123abcd-4567-89ef-0123-456789abcdef}", parsed));
    CHECK(parsed.ToString() == "0123abcd-4567-89ef-0123-456789abcdef");

    const uint8_t bytes[16] = { 0x01, 0x23, 0xab, 0xcd, 0x45, 0x67, 0x89, 0xef,
                                0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    CHECK(Guid::FromBytes(bytes) == parsed);

    Guid bad;
    CHECK(!Guid::Parse("0123abcd-4567-89ef-0123", bad));
    CHECK(!Guid::Parse("0123abcd_4567-89ef-0123-456789abcdef", bad));
    CHECK(!Guid::Parse("0123abcd-4567-89ef-0123-456789abcdeg", bad));
}

TEST_CASE("guid: generated guids are valid version 4 and distinct")
{
    const Guid a = Guid::Generate();
    const Guid b = Guid::Generate();
    CHECK(a.IsValid());
    CHECK(a != b);
    CHECK(a.ToString()[14] == '4');
}

TEST_CASE("entities: runtime entities get unique guids, map guids can be assigned")
{
    EntityManager entities;
    Entity* a = entities.CreateEntity<Plain>();
    Entity* b = entities.CreateEntity<Plain>();
    CHECK(a->GetGuid().IsValid());
    CHECK(a->GetGuid() != b->GetGuid());
    CHECK(entities.GetEntityByGuid(a->GetGuid()) == a);

    Guid saved;
    REQUIRE(Guid::Parse("11111111-2222-4333-8444-555555555555", saved));
    const Guid old = a->GetGuid();
    CHECK(entities.SetEntityGuid(*a, saved));
    CHECK(entities.GetEntityByGuid(saved) == a);
    CHECK(entities.GetEntityByGuid(old) == nullptr);
    CHECK(!entities.SetEntityGuid(*b, saved));   // taken
    CHECK(b->GetGuid() != saved);

    a->Destroy();
    CHECK(entities.GetEntityByGuid(saved) == nullptr);
    entities.DestroyPending();
    CHECK(entities.SetEntityGuid(*b, saved));    // free again
}

TEST_CASE("mapscript: an output delivers the linked input on the next update")
{
    Fixture f;
    Lamp* source = f.entities.CreateEntity<Lamp>();
    Lamp* target = f.entities.CreateEntity<Lamp>();
    Plain* player = f.entities.CreateEntity<Plain>();
    f.Link(*source, "OnLit", *target, "SetLevel");

    source->Fire("OnLit", player, "0.5");
    CHECK(target->received.empty());   // queued, not delivered inside FireOutput
    f.script.Update(0.0f);
    REQUIRE(target->received.size() == 1);
    CHECK(target->received[0].input == "SetLevel");
    CHECK(target->received[0].value == "0.5");
    CHECK(target->received[0].caller == source);
    CHECK(target->received[0].activator == player);
}

TEST_CASE("mapscript: a link parameter overrides the output value")
{
    Fixture f;
    Lamp* source = f.entities.CreateEntity<Lamp>();
    Lamp* target = f.entities.CreateEntity<Lamp>();
    f.Link(*source, "OnLit", *target, "SetLevel", "2");

    source->Fire("OnLit", nullptr, "0.5");
    f.script.Update(0.0f);
    REQUIRE(target->received.size() == 1);
    CHECK(target->received[0].value == "2");
}

TEST_CASE("mapscript: delays and once")
{
    Fixture f;
    Lamp* source = f.entities.CreateEntity<Lamp>();
    Lamp* target = f.entities.CreateEntity<Lamp>();
    f.Link(*source, "OnLit", *target, "TurnOn", "", 1.0f, true);

    source->Fire("OnLit");
    source->Fire("OnLit");   // the once link is gone after the first firing
    CHECK(f.script.GetLinkCount() == 0);
    f.script.Update(0.5f);
    CHECK(target->received.empty());
    f.script.Update(0.6f);
    CHECK(target->received.size() == 1);
    f.script.Update(5.0f);
    CHECK(target->received.size() == 1);
}

TEST_CASE("mapscript: chains advance one step per update")
{
    Fixture f;
    Lamp* a = f.entities.CreateEntity<Lamp>();
    Lamp* b = f.entities.CreateEntity<Lamp>();
    f.Link(*a, "OnLit", *b, "TurnOn");
    f.Link(*b, "OnLit", *a, "TurnOn");   // a loop must not hang the frame

    a->Fire("OnLit");
    f.script.Update(0.0f);
    CHECK(b->received.size() == 1);
    CHECK(a->received.empty());
    f.script.Update(0.0f);
    CHECK(a->received.size() == 1);
    CHECK(f.script.GetPendingEventCount() == 1);
}

TEST_CASE("mapscript: Kill, missing targets and unscriptable targets")
{
    Fixture f;
    Lamp* source = f.entities.CreateEntity<Lamp>();
    Lamp* victim = f.entities.CreateEntity<Lamp>();
    Plain* plain = f.entities.CreateEntity<Plain>();
    f.Link(*source, "OnLit", *victim, "Kill");
    f.Link(*source, "OnLit", *plain, "TurnOn");

    source->Fire("OnLit");
    f.script.Update(0.0f);
    CHECK(victim->IsPendingDestroy());
    CHECK(!plain->IsPendingDestroy());

    f.entities.DestroyPending();
    source->Fire("OnLit");   // the victim is gone: nothing to deliver to, no crash
    f.script.Update(0.0f);
    CHECK(f.script.GetPendingEventCount() == 0);

    CHECK(!f.script.SendInput(*source, "Unknown"));
    CHECK(f.script.SendInput(*source, "SetLevel", nullptr, nullptr, "3"));
    CHECK(source->received.back().value == "3");
}

TEST_CASE("mapscript: ports include the built-in Kill input")
{
    Lamp lamp;
    const MapScriptPorts& ports = lamp.GetMapScriptPorts();
    CHECK(ports.HasInput("TurnOn"));
    CHECK(ports.HasInput("Kill"));
    CHECK(!ports.HasInput("OnLit"));
    CHECK(ports.HasOutput("OnLit"));
}

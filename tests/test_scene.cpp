#include "test_framework.h"
#include "ent_trigger.h"
#include "entity_manager.h"
#include "game_context.h"
#include "scene.h"

TEST_CASE("scene: objects stay findable after removals")
{
    Scene scene;
    const uint32_t a = scene.CreateObject({});
    const uint32_t b = scene.CreateObject({});
    const uint32_t c = scene.CreateObject({});

    CHECK(scene.RemoveObject(a));
    CHECK(!scene.RemoveObject(a));
    CHECK(scene.FindObject(a) == nullptr);
    REQUIRE(scene.FindObject(b) != nullptr);
    REQUIRE(scene.FindObject(c) != nullptr);
    CHECK(scene.FindObject(b)->id == b);
    CHECK(scene.FindObject(c)->id == c);
    CHECK(scene.GetObjects().size() == 2);

    CHECK(scene.RemoveObject(c));
    CHECK(scene.FindObject(b)->id == b);

    scene.Clear();
    CHECK(scene.FindObject(b) == nullptr);
}

TEST_CASE("trigger: only tagged entities count, unless the filter is empty")
{
    GameContext context;
    EntityManager manager;
    context.entities = &manager;
    manager.SetGameContext(&context);

    auto* trigger = manager.CreateEntity<TriggerEntity>();
    trigger->ApplyProperty("Size", "10 10 10");

    Entity* prop = manager.CreateEntity<Entity>();
    Entity* player = manager.CreateEntity<Entity>();
    player->Tags.push_back(kPlayerTag);

    manager.Update(0.0f);
    CHECK(trigger->GetOverlapCount() == 1);

    player->Position = { 100.0f, 0.0f, 0.0f };
    manager.Update(0.0f);
    CHECK(trigger->GetOverlapCount() == 0);

    trigger->ApplyProperty("Filter", "\"\"");
    prop->Position = {};
    manager.Update(0.0f);
    CHECK(trigger->GetOverlapCount() == 1);
}

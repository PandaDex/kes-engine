#include "test_framework.h"
#include "camera.h"
#include "ent_post_process_volume.h"
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

TEST_CASE("post process: layers blend overridden fields by priority and weight")
{
    Scene scene;
    scene.GetPostProcessDefaults().bloomIntensity = 0.1f;
    scene.GetPostProcessDefaults().vignetteIntensity = 0.0f;

    PostProcessLayer vignette;
    vignette.overrides = PostProcessOverride_VignetteIntensity;
    vignette.settings.vignetteIntensity = 1.0f;
    vignette.settings.bloomIntensity = 0.9f; // not overridden, ignored
    vignette.weight = 0.5f;
    const PostProcessLayerId id = scene.AddPostProcessLayer(vignette);
    scene.ResolvePostProcess();
    CHECK_NEAR(scene.GetPostProcess().vignetteIntensity, 0.5f, 1e-5);
    CHECK_NEAR(scene.GetPostProcess().bloomIntensity, 0.1f, 1e-5);

    // Higher priority applies last, whatever the order.
    PostProcessLayer low;
    low.overrides = PostProcessOverride_BloomIntensity | PostProcessOverride_AutoExposure;
    low.settings.bloomIntensity = 0.2f;
    low.settings.autoExposure = false;
    low.priority = 10;
    PostProcessLayer high = low;
    high.settings.bloomIntensity = 0.4f;
    high.settings.autoExposure = true;
    high.priority = 20;
    high.weight = 0.4f; // booleans switch above 0.5
    scene.SubmitPostProcessLayer(high);
    scene.SubmitPostProcessLayer(low);
    scene.ResolvePostProcess();
    CHECK_NEAR(scene.GetPostProcess().bloomIntensity, 0.2f + (0.4f - 0.2f) * 0.4f, 1e-5);
    CHECK(!scene.GetPostProcess().autoExposure);

    // Submitted layers are per frame; code layers survive Clear.
    scene.Clear();
    scene.ResolvePostProcess();
    CHECK_NEAR(scene.GetPostProcess().bloomIntensity, 0.1f, 1e-5);
    CHECK_NEAR(scene.GetPostProcess().vignetteIntensity, 0.5f, 1e-5);
    REQUIRE(scene.FindPostProcessLayer(id) != nullptr);
    scene.FindPostProcessLayer(id)->weight = 1.0f;
    scene.ResolvePostProcess();
    CHECK_NEAR(scene.GetPostProcess().vignetteIntensity, 1.0f, 1e-5);
    CHECK(scene.RemovePostProcessLayer(id));
    scene.ResolvePostProcess();
    CHECK_NEAR(scene.GetPostProcess().vignetteIntensity, 0.0f, 1e-5);
}

TEST_CASE("post process volume: fades in over the blend distance")
{
    GameContext context;
    EntityManager manager;
    Scene scene;
    Camera camera;
    context.entities = &manager;
    context.scene = &scene;
    context.mainCamera = &camera;
    manager.SetGameContext(&context);

    auto* volume = manager.CreateEntity<PostProcessVolumeEntity>();
    volume->ApplyProperty("Size", "2 2 2");
    volume->ApplyProperty("BlendDistance", "4");
    volume->ApplyProperty("VignetteIntensity", "0.8");
    volume->ApplyProperty("BloomIntensity", ""); // empty: not overridden

    auto resolveAt = [&](float x)
    {
        camera.x = x;
        camera.y = 0.0f;
        camera.z = 0.0f;
        scene.ClearSubmittedPostProcessLayers();
        manager.Update(0.0f);
        scene.ResolvePostProcess();
        return scene.GetPostProcess().vignetteIntensity;
    };
    CHECK_NEAR(resolveAt(0.0f), 0.8f, 1e-5);
    CHECK_NEAR(resolveAt(3.0f), 0.8f * 0.5f, 1e-4); // 2 m outside the box
    CHECK_NEAR(resolveAt(10.0f), 0.0f, 1e-5);
    CHECK_NEAR(scene.GetPostProcess().bloomIntensity, PostProcessSettings{}.bloomIntensity, 1e-6); // not set by the map

    volume->ApplyProperty("Unbound", "1");
    CHECK_NEAR(resolveAt(100.0f), 0.8f, 1e-5);
}

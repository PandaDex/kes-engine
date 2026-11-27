#include "test_framework.h"
#include <cmath>
#include <cstring>
#include "camera.h"
#include "ent_post_process_volume.h"
#include "ent_trigger.h"
#include "entity_manager.h"
#include "game_context.h"
#include "render_common.h"
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

TEST_CASE("post process: grading fields blend and the volume parses them")
{
    PostProcessVolumeEntity volume;
    CHECK(volume.ApplyProperty("Saturation", "0.5"));
    CHECK(volume.ApplyProperty("Gain", "2 1 1"));
    CHECK(volume.ApplyProperty("Tonemapper", "1"));
    CHECK(volume.ApplyProperty("Sharpen", ""));
    const PostProcessLayer& layer = volume.GetLayer();
    CHECK(layer.overrides == (PostProcessOverride_Saturation | PostProcessOverride_Gain | PostProcessOverride_Tonemapper));

    Scene scene;
    PostProcessLayer half = layer;
    half.weight = 0.5f;
    scene.SubmitPostProcessLayer(half);
    scene.ResolvePostProcess();
    const PostProcessSettings& post = scene.GetPostProcess();
    CHECK_NEAR(post.saturation, 0.75f, 1e-5);
    CHECK_NEAR(post.gain[0], 1.5f, 1e-5);
    CHECK_NEAR(post.gain[1], 1.0f, 1e-5);
    CHECK(post.tonemapper == Tonemapper::ACES); // switches above 0.5
    CHECK_NEAR(post.sharpen, 0.0f, 1e-5);
}

TEST_CASE("post process: neutral white balance is the identity")
{
    float lms[3];
    render::ComputeWhiteBalanceLms(0.0f, 0.0f, lms);
    CHECK_NEAR(lms[0], 1.0f, 1e-6);
    render::ComputeWhiteBalanceLms(0.5f, 0.0f, lms);
    CHECK(lms[2] < lms[0]); // warmer: less blue
}

TEST_CASE("screen-space lighting: inverse view-projection and the on/off rules")
{
    const float m[16] = { 1.3f, 0.0f, 0.2f, 0.0f, 0.1f, 1.7f, 0.0f, 0.0f, 0.0f, 0.3f, 1.001f, 1.0f, 0.5f, -2.0f, 3.0f, 0.0f };
    float inv[16];
    REQUIRE(render::InvertMatrix(m, inv));
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += m[row * 4 + k] * inv[k * 4 + col];
            CHECK_NEAR(sum, row == col ? 1.0f : 0.0f, 1e-4);
        }
    }

    render::SceneCBData cb = {};
    cb.contactShadowParams[2] = 0.0f; // a directional light exists
    PostProcessSettings post;
    GraphicsOptions options;
    const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    render::ScreenSpaceAvailability all;
    all.lighting = all.reflections = all.depthOfField = all.temporalAA = all.temporalHistory = all.motionBlur = true;
    all.prevViewProj = identity;
    render::SetScreenSpaceLighting(cb, post, options, 1280, 720, all);
    CHECK(render::WantsDepthPrepass(cb));
    CHECK(cb.ssaoParams[2] > 0.5f);
    CHECK(cb.ssaoParams[3] > 0.5f);
    CHECK(cb.ssrParams[2] > 0.5f);

    options.ambientOcclusion = false;
    post.contactShadowLength = 0.0f;
    render::ScreenSpaceAvailability noHistory = all;
    noHistory.prevViewProj = nullptr;
    noHistory.temporalAA = false;
    render::SetScreenSpaceLighting(cb, post, options, 1280, 720, noHistory);
    CHECK(cb.ssrParams[2] < 0.5f);
    CHECK(!render::WantsDepthPrepass(cb));

    options = GraphicsOptions();
    post = PostProcessSettings();
    render::ScreenSpaceAvailability none;
    none.prevViewProj = identity;
    render::SetScreenSpaceLighting(cb, post, options, 1280, 720, none); // no targets
    CHECK(!render::WantsDepthPrepass(cb));

    // TAA jitter stays within half a pixel and changes the matrix the draws use.
    render::FrameSetup frame;
    std::memcpy(frame.viewProj.m, identity, sizeof(identity));
    render::ApplyTemporalJitter(frame, 3, 1280, 720);
    CHECK(std::fabs(frame.scene.taaParams[0]) <= 0.5f / 1280.0f + 1e-7f);
    CHECK(std::fabs(frame.scene.taaParams[1]) <= 0.5f / 720.0f + 1e-7f);
    CHECK(frame.viewProj.m[12] != 0.0f);
}

#include "test_framework.h"
#include "camera.h"
#include "render_common.h"
#include "scene.h"

namespace
{
    render::FrameSetup BuildFrame(const Scene& scene)
    {
        Camera camera;
        camera.x = camera.y = camera.z = 0.0f; // looking down +Z
        render::FrameSetup frame;
        SceneState state;
        render::BuildFrameSetup(frame, state, camera, scene, 16.0f / 9.0f, GraphicsOptions{});
        return frame;
    }
}

TEST_CASE("lights: local lights go to the clusters, directional lights to the constants")
{
    Scene scene;
    Light sun;
    sun.type = LightType::Directional;
    scene.GetLights().push_back(sun);
    for (int i = 0; i < 200; ++i)
    {
        Light light;
        light.type = i % 2 ? LightType::Spot : LightType::Point;
        light.range = 1.0f;
        light.transform.position = Vector3(static_cast<float>(i % 20) - 10.0f, static_cast<float>(i / 20) - 5.0f, 30.0f);
        scene.GetLights().push_back(light);
    }
    Light behind;
    behind.transform.position = Vector3(0.0f, 0.0f, -30.0f);
    behind.range = 1.0f;
    scene.GetLights().push_back(behind);

    const render::FrameSetup frame = BuildFrame(scene);
    CHECK_NEAR(frame.scene.lightCounts[0], 1.0f, 1e-6);
    CHECK(frame.clusterLights.size() == 200u);
    CHECK_NEAR(frame.scene.clusterParams[3], 200.0f, 1e-6);
    CHECK(frame.clusterGrid.size() > static_cast<size_t>(render::kClusterCount));
    CHECK(frame.clusterGrid.size() <= static_cast<size_t>(render::kClusterGridUints));
}

TEST_CASE("lights: scene lights come before particle lights and keep nearest first")
{
    Scene scene;
    Light farLight;
    farLight.transform.position = Vector3(0.0f, 0.0f, 50.0f);
    scene.GetLights().push_back(farLight);
    Light nearLight;
    nearLight.transform.position = Vector3(0.0f, 0.0f, 20.0f);
    scene.GetLights().push_back(nearLight);
    ScenePointLight particle = {};
    particle.position[2] = 10.0f;
    particle.radius = 2.0f;
    particle.color[0] = 1.0f;
    scene.GetParticles().lights.push_back(particle);

    const render::FrameSetup frame = BuildFrame(scene);
    REQUIRE(frame.clusterLights.size() == 3u);
    CHECK_NEAR(frame.clusterLights[0].position[2], 20.0f, 1e-6);
    CHECK_NEAR(frame.clusterLights[1].position[2], 50.0f, 1e-6);
    CHECK_NEAR(frame.clusterLights[2].position[2], 10.0f, 1e-6);
    CHECK_NEAR(frame.clusterLights[2].params[2], -1.0f, 1e-6);
}

TEST_CASE("lights: a narrow spot is culled by its cone, not its range sphere")
{
    Scene scene;
    Light spot;
    spot.type = LightType::Spot;
    spot.spotAngle = 20.0f;
    spot.range = 20.0f;
    // Range sphere reaches into the view, the cone points away from it.
    spot.transform.position = Vector3(0.0f, 0.0f, -5.0f);
    spot.transform.rotation = Vector3(0.0f, 3.14159265f, 0.0f); // radians, facing -Z
    scene.GetLights().push_back(spot);

    const render::FrameSetup frame = BuildFrame(scene);
    CHECK(frame.clusterLights.empty());
}

namespace
{
    struct ShadowRig
    {
        Scene scene;
        Camera camera;
        render::FrameSetup frame;
        GraphicsOptions options;
        std::vector<render::ShadowCaster> casters;

        ShadowRig() { camera.x = camera.y = camera.z = 0.0f; }

        // Local shadow views drawn this frame.
        int Step()
        {
            SceneState state;
            render::BuildFrameSetup(frame, state, camera, scene, 16.0f / 9.0f, options);
            render::UpdateLocalShadows(frame, casters);
            int local = 0;
            for (const render::ShadowView& view : frame.shadowViews)
                local += view.cascade ? 0 : 1;
            return local;
        }
    };

    Light ShadowSpot(uint32_t id, float x, float z)
    {
        Light light;
        light.id = id;
        light.type = LightType::Spot;
        light.castShadows = true;
        light.range = 6.0f;
        light.spotAngle = 60.0f;
        light.transform.position = Vector3(x, 3.0f, z);
        light.transform.rotation = Vector3(-1.5707963f, 0.0f, 0.0f);
        return light;
    }

    bool TilesOverlap(const render::ShadowView& a, const render::ShadowView& b)
    {
        return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
    }
}

TEST_CASE("shadows: every shadowed light gets its own atlas tile")
{
    ShadowRig rig;
    for (uint32_t i = 0; i < 40; ++i)
        rig.scene.GetLights().push_back(ShadowSpot(i + 1, static_cast<float>(i % 8) * 3.0f - 10.5f, 10.0f + static_cast<float>(i / 8) * 4.0f));
    Light point;
    point.id = 100;
    point.castShadows = true;
    point.transform.position = Vector3(0.0f, 1.0f, 6.0f);
    rig.scene.GetLights().push_back(point);

    REQUIRE(rig.Step() == 46); // 40 spots + 6 cube faces
    int shadowed = 0;
    for (const render::GPULight& light : rig.frame.clusterLights)
        shadowed += light.params[2] >= 0.0f ? 1 : 0;
    CHECK(shadowed == 41);
    for (size_t a = 0; a < rig.frame.shadowViews.size(); ++a)
        for (size_t b = a + 1; b < rig.frame.shadowViews.size(); ++b)
            CHECK(!TilesOverlap(rig.frame.shadowViews[a], rig.frame.shadowViews[b]));
    // The point light's faces share one size; a nearer light gets a bigger tile than a far one.
    const render::GPULight& nearest = rig.frame.clusterLights[0];
    REQUIRE(nearest.params[0] == 1.0f);
    const int first = static_cast<int>(nearest.params[2]);
    for (int face = 1; face < 6; ++face)
        CHECK(rig.frame.scene.localShadowRects[first + face][0] == rig.frame.scene.localShadowRects[first][0]);
    const render::GPULight& farthest = rig.frame.clusterLights[40];
    CHECK(rig.frame.scene.localShadowRects[static_cast<int>(rig.frame.clusterLights[1].params[2])][0] >=
          rig.frame.scene.localShadowRects[static_cast<int>(farthest.params[2])][0]);
}

TEST_CASE("shadows: cached tiles are drawn again only when something in range changes")
{
    ShadowRig rig;
    rig.scene.GetLights().push_back(ShadowSpot(1, -4.0f, 12.0f));
    rig.scene.GetLights().push_back(ShadowSpot(2, 4.0f, 12.0f));
    render::ShadowCaster box;
    box.key = 7;
    box.state = 1;
    box.center = Vector3(-4.0f, 0.5f, 12.0f);
    box.radius = 1.0f;
    rig.casters.push_back(box);

    CHECK(rig.Step() == 2); // first frame: both
    CHECK(rig.Step() == 0); // nothing changed
    rig.casters[0].state = 2; // moved, but only under light 1
    rig.casters[0].center = Vector3(-4.0f, 0.5f, 12.5f);
    CHECK(rig.Step() == 1);
    CHECK(rig.Step() == 0);
    rig.scene.GetLights()[1].transform.position.y = 3.5f; // light 2 moved
    CHECK(rig.Step() == 1);
    rig.casters.clear(); // the box is gone: light 1 again
    CHECK(rig.Step() == 1);
    CHECK(rig.Step() == 0);
}

TEST_CASE("shadows: lights that left the view give their tiles to new ones")
{
    ShadowRig rig;
    rig.options.shadowQuality = ShadowQuality::Low; // 2048 atlas
    for (uint32_t i = 0; i < 30; ++i)
        rig.scene.GetLights().push_back(ShadowSpot(i + 1, static_cast<float>(i % 6) * 2.0f - 5.0f, 4.0f + static_cast<float>(i / 6) * 2.0f));
    CHECK(rig.Step() == 30);

    // Same lights behind the camera, new ones in front: the old tiles get evicted.
    for (Light& light : rig.scene.GetLights())
        light.transform.position.z = -light.transform.position.z - 20.0f;
    for (uint32_t i = 0; i < 30; ++i)
        rig.scene.GetLights().push_back(ShadowSpot(i + 101, static_cast<float>(i % 6) * 2.0f - 5.0f, 4.0f + static_cast<float>(i / 6) * 2.0f));
    CHECK(rig.Step() == 30);
    int shadowed = 0;
    for (const render::GPULight& light : rig.frame.clusterLights)
        shadowed += light.params[2] >= 0.0f ? 1 : 0;
    CHECK(shadowed == 30);
    for (size_t a = 0; a < rig.frame.shadowViews.size(); ++a)
        for (size_t b = a + 1; b < rig.frame.shadowViews.size(); ++b)
            CHECK(!TilesOverlap(rig.frame.shadowViews[a], rig.frame.shadowViews[b]));
}

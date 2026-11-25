#include "test_framework.h"
#include <fstream>
#include "camera.h"
#include "particle_asset.h"
#include "particle_graph.h"
#include "particle_sim.h"
#include "render_common.h"
#include "scene.h"

namespace
{
    ParticleSystemData MakeFountain()
    {
        ParticleSystemData system;
        ParticleEmitterData e;
        e.name = "Sparks";
        e.maxParticles = 500;
        e.seed = 42;
        e.modules.push_back(MakeParticleModule("SpawnRate", ParticleStage::EmitterUpdate));
        e.modules.back().Find("Rate")->value = ParticleValue::Constant(100.0f);
        e.modules.push_back(MakeParticleModule("InitializeParticle", ParticleStage::ParticleSpawn));
        e.modules.back().Find("Lifetime")->value = ParticleValue::Constant(1.0f);
        e.modules.push_back(MakeParticleModule("AddVelocity", ParticleStage::ParticleSpawn));
        e.modules.push_back(MakeParticleModule("Gravity", ParticleStage::ParticleUpdate));
        e.modules.push_back(MakeParticleModule("ScaleColor", ParticleStage::ParticleUpdate));
        e.renderers.push_back(MakeParticleRenderer("Sprite"));
        e.renderers.push_back(MakeParticleRenderer("Light"));
        system.emitters.push_back(e);
        return system;
    }
}

TEST_CASE("particles: values round trip through text")
{
    ParticleValue v;
    CHECK(ParseParticleValue("random uniform 1 2 ; 3 4", 2, v));
    CHECK(v.mode == ParticleValueMode::Random);
    CHECK(v.uniform);
    CHECK_NEAR(v.b[1], 4.0f, 1e-6);

    CHECK(ParseParticleValue("gradient NormalizedAge ; 0 1 0 0 1 ; 1 0 0 1 0", 4, v));
    float c[4];
    EvaluateParticleGradient(v.gradient, 0.5f, c);
    CHECK_NEAR(c[0], 0.5f, 1e-5);
    CHECK_NEAR(c[2], 0.5f, 1e-5);

    ParticleValue back;
    CHECK(ParseParticleValue(FormatParticleValue(v, 4), 4, back));
    CHECK(back.mode == ParticleValueMode::Gradient);
    CHECK(back.gradient.size() == 2);

    CHECK(ParseParticleValue("curve Speed 2 ; 0 0 ; 1 1", 1, v));
    CHECK(v.input == ParticleCurveInput::Speed);
    CHECK_NEAR(v.a[0], 2.0f, 1e-6);
    CHECK_NEAR(EvaluateParticleCurve(v.curve, 1.0f), 1.0f, 1e-6);
}

TEST_CASE("particles: system file round trip keeps modules, renderers and graphs")
{
    ParticleSystemData system = MakeFountain();
    system.userParams.push_back({ "Heat", 2.0f });
    ParticleEmitterData& e = system.emitters[0];
    ParticleGraphData graph;
    graph.id = 3;
    graph.nodes.push_back({ 1, "Output", 0, 0, {}, {} });
    graph.nodes.push_back({ 2, "UserParameter", -200, 0, { { "Name", "Heat" } }, {} });
    graph.links.push_back({ 2, 0, 1, 0 });
    e.graphs.push_back(graph);
    e.modules[0].Find("Rate")->value.mode = ParticleValueMode::Graph;
    e.modules[0].Find("Rate")->value.graph = 3;

    kestest::TempDir dir;
    const std::string path = dir.File("fountain.kfx");
    {
        std::ofstream out(path, std::ios::binary);
        out << WriteParticleSystemText(system);
    }
    ParticleSystemData loaded;
    std::string error;
    REQUIRE(LoadParticleSystemFile(path.c_str(), loaded, &error));
    REQUIRE(loaded.emitters.size() == 1);
    const ParticleEmitterData& le = loaded.emitters[0];
    CHECK(le.name == "Sparks");
    CHECK(le.modules.size() == e.modules.size());
    CHECK(le.renderers.size() == 2);
    CHECK(le.renderers[1].type == "Light");
    REQUIRE(le.graphs.size() == 1);
    CHECK(le.graphs[0].nodes.size() == 2);
    CHECK(le.graphs[0].links.size() == 1);
    CHECK(le.modules[0].Find("Rate")->value.mode == ParticleValueMode::Graph);
    CHECK(loaded.userParams.size() == 1);
    CHECK(WriteParticleSystemText(loaded) == WriteParticleSystemText(system));
}

TEST_CASE("particles: graphs compile and evaluate")
{
    ParticleSystemData system;
    system.userParams.push_back({ "Heat", 3.0f });
    ParticleEmitterData e;
    ParticleGraphData g;
    g.nodes.push_back({ 1, "Output", 0, 0, {}, {} });
    g.nodes.push_back({ 2, "Multiply", 0, 0, {}, { { 1, { 2.0f } } } });
    g.nodes.push_back({ 3, "UserParameter", 0, 0, { { "Name", "Heat" } }, {} });
    g.nodes.push_back({ 4, "Vector", 0, 0, { { "X", "1" }, { "Y", "2" }, { "Z", "3" } }, {} });
    g.nodes.push_back({ 5, "Length", 0, 0, {}, {} });
    g.links.push_back({ 3, 0, 2, 0 });
    g.links.push_back({ 2, 0, 1, 0 });
    ParticleGraphProgram program;
    std::string error;
    REQUIRE(program.Compile(g, e, system, &error));
    ParticleEvalContext ctx;
    const float user = 3.0f;
    ctx.userValues = &user;
    float out[8];
    program.Run(ctx, 0, 2, out);
    CHECK_NEAR(out[0], 6.0f, 1e-6);
    CHECK_NEAR(out[7], 6.0f, 1e-6);

    g.links = { { 4, 0, 5, 0 }, { 5, 0, 1, 0 } };
    REQUIRE(program.Compile(g, e, system, &error));
    program.Run(ctx, 0, 1, out);
    CHECK_NEAR(out[0], std::sqrt(14.0f), 1e-5);

    g.links.push_back({ 1, 0, 5, 0 });
    g.nodes[0].type = "Add"; // no Output node left
    CHECK(!program.Compile(g, e, system, &error));
}

TEST_CASE("particles: simulation spawns, ages, falls and dies")
{
    ParticleSystemData system = MakeFountain();
    ParticleSystemInstance instance;
    instance.Init(&system, 7);
    instance.SetTransform({ 0.0f, 10.0f, 0.0f }, {}, { 1.0f, 1.0f, 1.0f }, true);
    instance.Activate(true);
    for (int i = 0; i < 30; ++i)
        instance.Tick(1.0f / 60.0f);
    const uint32_t count = instance.GetParticleCount();
    CHECK(count >= 45 && count <= 55); // 100/s for half a second

    const ParticleEmitterState& state = instance.GetEmitter(0);
    float minAge = 1e9f, maxAge = 0.0f;
    for (uint32_t i = 0; i < state.count; ++i)
    {
        minAge = std::min(minAge, state.streams[ParticleAttr::Age][i]);
        maxAge = std::max(maxAge, state.streams[ParticleAttr::Age][i]);
        CHECK(state.streams[ParticleAttr::Position + 1][i] > 9.0f);
    }
    CHECK(maxAge > 0.4f);
    CHECK(minAge < 0.05f);

    std::vector<ParticleSpriteInstance> sprites;
    instance.ExtractSprites(0, system.emitters[0].renderers[0], sprites);
    CHECK(sprites.size() == count);
    std::vector<ParticleLightInstance> lights;
    instance.ExtractLights(0, system.emitters[0].renderers[1], lights);
    CHECK(!lights.empty());
    CHECK(lights.size() <= 128);
    CHECK_NEAR(lights[0].radius, 3.0f, 1e-5);

    instance.Deactivate();
    for (int i = 0; i < 90; ++i)
        instance.Tick(1.0f / 60.0f);
    CHECK(instance.GetParticleCount() == 0);
    CHECK(instance.IsComplete());
}

TEST_CASE("particles: bursts fire once per loop and ground collision holds")
{
    ParticleSystemData system;
    ParticleEmitterData e;
    e.duration = 1.0f;
    e.loop = false;
    e.modules.push_back(MakeParticleModule("SpawnBurst", ParticleStage::EmitterUpdate));
    e.modules.back().Find("Count")->value = ParticleValue::Constant(25.0f);
    e.modules.push_back(MakeParticleModule("InitializeParticle", ParticleStage::ParticleSpawn));
    e.modules.back().Find("Lifetime")->value = ParticleValue::Constant(5.0f);
    e.modules.push_back(MakeParticleModule("Gravity", ParticleStage::ParticleUpdate));
    e.modules.push_back(MakeParticleModule("Collision", ParticleStage::ParticleUpdate));
    e.modules.back().Find("Mode")->text = "GroundPlane";
    system.emitters.push_back(e);

    ParticleSystemInstance instance;
    instance.Init(&system, 1);
    instance.SetTransform({ 0.0f, 1.0f, 0.0f }, {}, { 1.0f, 1.0f, 1.0f }, true);
    instance.Activate(true);
    for (int i = 0; i < 120; ++i)
        instance.Tick(1.0f / 60.0f);
    const ParticleEmitterState& state = instance.GetEmitter(0);
    CHECK(state.count == 25);
    for (uint32_t i = 0; i < state.count; ++i)
        CHECK(state.streams[ParticleAttr::Position + 1][i] >= 0.0f);
}

TEST_CASE("particles: cluster binning puts a light into the clusters it touches")
{
    Scene scene;
    ScenePointLight light = {};
    light.position[2] = 10.0f; // straight ahead of a camera at the origin looking down +Z
    light.radius = 2.0f;
    light.color[0] = 1.0f;
    scene.GetParticles().lights.push_back(light);
    ScenePointLight behind = light;
    behind.position[2] = -10.0f;
    scene.GetParticles().lights.push_back(behind);

    Camera camera;
    camera.x = camera.y = camera.z = 0.0f;
    render::FrameSetup frame;
    SceneState state;
    render::BuildFrameSetup(frame, state, camera, scene, 16.0f / 9.0f, GraphicsOptions{});
    REQUIRE(frame.clusterLights.size() == 1);
    CHECK_NEAR(frame.scene.clusterParams[3], 1.0f, 1e-6);

    // Center cluster at the light's depth.
    const float slice = std::floor(std::log(10.0f / frame.scene.clusterDepth[0]) * frame.scene.clusterDepth[1]);
    const int cluster = (static_cast<int>(slice) * render::kClusterY + render::kClusterY / 2) * render::kClusterX + render::kClusterX / 2;
    const uint32_t header = frame.clusterGrid[static_cast<size_t>(cluster)];
    CHECK((header & 0xFFu) == 1u);
    CHECK(frame.clusterGrid[render::kClusterCount + (header >> 8)] == 0u);
    // A corner cluster close to the camera doesn't see it.
    CHECK((frame.clusterGrid[0] & 0xFFu) == 0u);
}

TEST_CASE("particles: light budget keeps the same particles and the total energy")
{
    ParticleSystemData system;
    ParticleEmitterData e;
    e.seed = 5;
    e.modules.push_back(MakeParticleModule("SpawnBurst", ParticleStage::EmitterUpdate));
    e.modules.back().Find("Count")->value = ParticleValue::Constant(40.0f);
    e.modules.push_back(MakeParticleModule("InitializeParticle", ParticleStage::ParticleSpawn));
    e.modules.back().Find("Lifetime")->value = ParticleValue::Constant(10.0f);
    e.modules.back().Find("Color")->value = ParticleValue::Constant(1.0f, 0.5f, 0.25f, 1.0f);
    e.modules.push_back(MakeParticleModule("ShapeLocation", ParticleStage::ParticleSpawn));
    e.modules.back().Find("Radius")->value = ParticleValue::Constant(20.0f);
    e.modules.back().Find("SurfaceOnly")->text = "true";
    ParticleRendererData light = MakeParticleRenderer("Light");
    light.Find("MaxLights")->text = "8";
    e.renderers.push_back(light);
    system.emitters.push_back(e);

    ParticleSystemInstance instance;
    instance.Init(&system, 1);
    instance.SetTransform({}, {}, { 1.0f, 1.0f, 1.0f }, true);
    instance.Activate(true);
    instance.Tick(1.0f / 60.0f);
    REQUIRE(instance.GetParticleCount() == 40);

    std::vector<ParticleLightInstance> a, b;
    instance.ExtractLights(0, system.emitters[0].renderers[0], a);
    REQUIRE(a.size() == 8);
    float total = 0.0f;
    for (const ParticleLightInstance& l : a)
        total += l.color[0] + l.color[1] + l.color[2];
    CHECK_NEAR(total, 40.0f * 1.75f, 1e-3);

    instance.Tick(1.0f / 60.0f);
    instance.ExtractLights(0, system.emitters[0].renderers[0], b);
    REQUIRE(b.size() == 8);
    for (const ParticleLightInstance& la : a)
    {
        bool found = false;
        for (const ParticleLightInstance& lb : b)
            found = found || (std::fabs(la.position[0] - lb.position[0]) < 0.5f && std::fabs(la.position[2] - lb.position[2]) < 0.5f);
        CHECK(found);
    }
}

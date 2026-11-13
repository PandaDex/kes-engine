#include "test_framework.h"
#include "camera.h"
#include "ent_brush.h"
#include "ent_player.h"
#include "ent_skinned_model.h"
#include "file_paths.h"
#include "input.h"
#include "animation_clip.h"
#include "kmodel.h"
#include "world.h"

// Runs the real startup map without a window or renderer. The map is edited
// freely, so nothing here assumes its layout beyond there being something to
// stand on below the player start.
namespace
{
    constexpr float kFixedDt = 1.0f / 50.0f;

    std::string GameContentDirectory()
    {
        const std::string roots = KES_TEST_CONTENT_DIRS;
        return roots.substr(0, roots.find('|'));
    }

    struct LoadedWorld
    {
        Camera camera;
        InputState input;
        World world;

        LoadedWorld()
        {
            SetAssetRootOverride(GameContentDirectory().c_str());
            REQUIRE(world.Init(&camera, &input));
            REQUIRE(world.LoadMap("maps/startup.fmapc"));
        }

        ~LoadedWorld()
        {
            world.Shutdown();
            SetAssetRootOverride("");
        }

        CollisionWorld& Collision() { return *world.GetGameContext().collision; }
        Entity& Player() { return *world.GetEntities().GetEntityOfClass<EnginePlayerEntity>(); }

        void Run(int steps)
        {
            for (int step = 0; step < steps; ++step)
            {
                world.FixedUpdate(kFixedDt);
                world.Update(kFixedDt);
            }
        }

        // A point clear of the player, above something solid. Returns the
        // height of that surface.
        float FindDropPoint(Vector3& outPoint)
        {
            const Vector3 player = Player().Position;
            for (float side : { 2.0f, -2.0f, 4.0f, -4.0f })
            {
                outPoint = { player.x + side, player.y + 3.0f, player.z };
                std::vector<RaycastHit> hits;
                Collision().RaycastAll({ outPoint, { 0.0f, -1.0f, 0.0f } }, hits);
                if (!hits.empty() && hits[0].distance > 1.5f && hits[0].normal.y > 0.7f)
                    return hits[0].point.y;
            }
            return -1e9f;
        }
    };

    Vector3 RunBallScene()
    {
        LoadedWorld loaded;
        Vector3 start;
        REQUIRE(loaded.FindDropPoint(start) > -1e8f);

        SkinnedModelEntity* balls[3] = {};
        for (int i = 0; i < 3; ++i)
        {
            balls[i] = loaded.world.GetEntities().CreateEntity<SkinnedModelEntity>();
            balls[i]->ApplyProperty("Model", "meshes/ball.kmodel");
            balls[i]->ApplyProperty("Collider", "Sphere");
            balls[i]->ApplyProperty("Dynamic", "true");
            balls[i]->ApplyProperty("Restitution", "0.5");
            balls[i]->ApplyProperty("Scale", "0.25 0.25 0.25");
            balls[i]->Teleport(start + Vector3{ i * 0.3f, i * 1.0f, 0.0f });
        }
        loaded.Run(400);
        return balls[1]->Position + balls[2]->Position * 3.0f;
    }
}

TEST_CASE("world: the mover can't pass a surface of the startup map")
{
    LoadedWorld loaded;
    REQUIRE(loaded.world.GetEntities().GetEntityOfClass<EnginePlayerEntity>() != nullptr);

    Vector3 position;
    const float surface = loaded.FindDropPoint(position);
    REQUIRE(surface > -1e8f);

    position.y = surface + 4.0f;
    for (int step = 0; step < 100; ++step)
        position = loaded.Collision().MoveCapsule(position, { 0.0f, -0.25f, 0.0f }, 0.35f, 0.9f);
    CHECK_NEAR(position.y, surface + 0.35 + 0.9, 0.05);
}

TEST_CASE("world: a dynamic model falls onto the map and stays there")
{
    LoadedWorld loaded;
    Vector3 start;
    const float surface = loaded.FindDropPoint(start);
    REQUIRE(surface > -1e8f);

    auto* ball = loaded.world.GetEntities().CreateEntity<SkinnedModelEntity>();
    ball->ApplyProperty("Model", "meshes/ball.kmodel");
    ball->ApplyProperty("Collider", "Sphere");
    ball->ApplyProperty("Dynamic", "true");
    ball->ApplyProperty("Scale", "0.25 0.25 0.25");
    ball->Teleport(start);

    loaded.Run(300);
    CHECK(ball->Position.y < start.y - 0.5f);
    CHECK(ball->Position.y > surface);
}

TEST_CASE("world: a model's own colliders make it a dynamic body")
{
    LoadedWorld loaded;
    Vector3 start;
    const float surface = loaded.FindDropPoint(start);
    REQUIRE(surface > -1e8f);

    kestest::TempDir dir;
    KModelData model;
    REQUIRE(LoadKModelFile((GameContentDirectory() + "/meshes/ball.kmodel").c_str(), model));
    KModelCollider collider;
    collider.shape = KModelColliderShape::Sphere;
    collider.radius = 0.3f;
    model.colliders.push_back(collider);
    const std::string path = dir.File("ball_with_collider.kmodel");
    REQUIRE(SaveKModelFile(path.c_str(), model));

    auto* ball = loaded.world.GetEntities().CreateEntity<SkinnedModelEntity>();
    ball->ApplyProperty("Model", path);
    ball->ApplyProperty("Dynamic", "true");
    ball->Teleport(start);

    loaded.Run(300);
    CHECK(ball->Position.y < start.y - 0.5f);
    CHECK_NEAR(ball->Position.y, surface + 0.3, 0.05);
}

TEST_CASE("world: colliders on bones follow the animation")
{
    LoadedWorld loaded;
    kestest::TempDir dir;

    // "upper" sits 1 unit above "lower" and the clip bends it 90 degrees about Z,
    // so a sphere at its tip moves from (0, 2, 0) to (-1, 1, 0).
    KModelData model;
    KModelMesh mesh;
    mesh.vertices.resize(3);
    mesh.vertices[1].px = 0.1f;
    mesh.vertices[2].py = 2.0f;
    mesh.indices = { 0, 1, 2 };
    mesh.primitives.push_back({ 0, 3, -1 });
    model.meshes.push_back(mesh);
    KModelNode meshNode;
    meshNode.meshIndex = 0;
    KModelNode lower;
    lower.name = "lower";
    lower.boneIndex = 0;
    KModelNode upper;
    upper.name = "upper";
    upper.parentIndex = 1;
    upper.boneIndex = 1;
    upper.translation[1] = 1.0f;
    model.nodes = { meshNode, lower, upper };
    KModelBone lowerBone;
    lowerBone.name = "lower";
    lowerBone.nodeIndex = 1;
    KModelBone upperBone;
    upperBone.name = "upper";
    upperBone.nodeIndex = 2;
    upperBone.parentBoneIndex = 0;
    upperBone.inverseBindMatrix[13] = -1.0f;
    model.bones = { lowerBone, upperBone };
    KModelCollider tip;
    tip.shape = KModelColliderShape::Sphere;
    tip.boneIndex = 1;
    tip.center[1] = 1.0f;
    tip.radius = 0.2f;
    model.colliders.push_back(tip);
    const std::string modelPath = dir.File("bender.kmodel");
    REQUIRE(SaveKModelFile(modelPath.c_str(), model));

    AnimationClipData clip;
    clip.duration = 1.0f;
    clip.looping = false;
    AnimationNodeTrack track;
    track.nodeIndex = 2;
    track.rotationKeys = { { 0.0f, { 0.0f, 0.0f, 0.0f, 1.0f } }, { 1.0f, { 0.0f, 0.0f, 0.70710678f, 0.70710678f } } };
    clip.tracks.push_back(track);
    const std::string clipPath = dir.File("bend.kanim");
    REQUIRE(SaveAnimationClipFile(clipPath.c_str(), clip));

    const Vector3 origin = { 500.0f, 500.0f, 500.0f };
    auto* bender = loaded.world.GetEntities().CreateEntity<SkinnedModelEntity>();
    bender->ApplyProperty("Model", modelPath);
    bender->ApplyProperty("AnimClip", clipPath);
    bender->Teleport(origin);
    loaded.Run(1);

    const auto hitAt = [&](float x, float y)
    {
        RaycastHit hit;
        const bool found = loaded.Collision().Raycast({ { origin.x + x, origin.y + y + 3.0f, origin.z }, { 0.0f, -1.0f, 0.0f } }, hit);
        return found && bender->OwnsCollider(hit.other) ? origin.y + y + 3.0f - hit.distance : -1e9f;
    };
    CHECK_NEAR(hitAt(0.0f, 2.0f), origin.y + 2.2f, 0.05);

    loaded.Run(75);
    CHECK_NEAR(hitAt(-1.0f, 1.0f), origin.y + 1.2f, 0.05);
    CHECK(hitAt(0.0f, 2.0f) < -1e8f);
}

TEST_CASE("world: a dynamic brush falls and its scene object follows")
{
    LoadedWorld loaded;
    Vector3 start;
    const float surface = loaded.FindDropPoint(start);
    REQUIRE(surface > -1e8f);

    MapEntityGeometry geometry;
    MapEntityGeometry::Hull hull;
    hull.planes = {
        { { 1.0f, 0.0f, 0.0f }, 0.25f }, { { -1.0f, 0.0f, 0.0f }, 0.25f },
        { { 0.0f, 1.0f, 0.0f }, 0.25f }, { { 0.0f, -1.0f, 0.0f }, 0.25f },
        { { 0.0f, 0.0f, 1.0f }, 0.25f }, { { 0.0f, 0.0f, -1.0f }, 0.25f },
    };
    hull.boundsMin = { -0.25f, -0.25f, -0.25f };
    hull.boundsMax = { 0.25f, 0.25f, 0.25f };
    geometry.hulls.push_back(hull);
    geometry.meshes.push_back({ loaded.world.GetAssets().CreateCubeMesh(), 0 });

    auto* brush = loaded.world.GetEntities().CreateEntity<BrushEntity>();
    brush->ApplyProperty("Dynamic", "1");
    brush->ApplyMapGeometry(geometry);
    brush->Teleport(start);

    const size_t objectsBefore = loaded.world.GetScene().GetObjects().size();
    loaded.Run(150);
    CHECK(loaded.world.GetScene().GetObjects().size() == objectsBefore + 1);
    CHECK_NEAR(brush->Position.y, surface + 0.25, 0.03);

    bool followed = false;
    for (const SceneObject& object : loaded.world.GetScene().GetObjects())
        followed = followed || (object.transform.position.y == brush->Position.y && object.transform.position.x == brush->Position.x);
    CHECK(followed);

    brush->Destroy();
    loaded.Run(1);
    CHECK(loaded.world.GetScene().GetObjects().size() == objectsBefore);
}

TEST_CASE("world: the same scene simulates identically twice")
{
    const Vector3 first = RunBallScene();
    const Vector3 second = RunBallScene();
    CHECK(first.x == second.x);
    CHECK(first.y == second.y);
    CHECK(first.z == second.z);
}

TEST_CASE("mover: a capsule inside a brush can walk out of it")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f });
    world.AddBox({ -8.0f, 0.0f, 0.0f }, { 1.0f, 5.0f, 5.0f });   // a wall to stop at

    Vector3 position = {};
    REQUIRE(world.CheckCapsule(position, 0.35f, 0.9f));
    for (int step = 0; step < 150; ++step)
        position = world.MoveCapsule(position, { -0.1f, 0.0f, 0.0f }, 0.35f, 0.9f);
    CHECK(!world.CheckCapsule(position, 0.34f, 0.89f));
    CHECK_NEAR(position.x, -7.0 + 0.35, 0.05);
}

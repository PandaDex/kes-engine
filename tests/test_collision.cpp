#include "test_framework.h"
#include "collision_world.h"

namespace
{
    Ray RayAlongZ(float startZ)
    {
        return { { 0.0f, 0.0f, startZ }, { 0.0f, 0.0f, 1.0f } };
    }
}

TEST_CASE("collision: ray hits the near face of a box")
{
    CollisionWorld world;
    const ColliderHandle box = world.AddBox({ 0.0f, 0.0f, 5.0f }, { 1.0f, 1.0f, 1.0f });

    RaycastHit hit;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit));
    CHECK(hit.other == box);
    CHECK_NEAR(hit.distance, 4.0, 1e-4);
    CHECK_NEAR(hit.point.z, 4.0, 1e-4);
    CHECK_NEAR(hit.normal.z, -1.0, 1e-4);
}

TEST_CASE("collision: overlap checks ignore triggers")
{
    CollisionWorld world;
    world.AddBox({}, { 1.0f, 1.0f, 1.0f }, true);
    CHECK(!world.CheckSphere({}, 0.5f));
    CHECK(!world.CheckCapsule({}, 0.5f, 1.0f));
}

TEST_CASE("collision: a capsule sweep reports the surface it hit")
{
    // A hull whose near face leans 45 degrees towards -X and -Z.
    const float d = 0.70710678f;
    const std::vector<CollisionPlane> planes = {
        { { -d, 0.0f, -d }, -5.0f },
        { { 1.0f, 0.0f, 0.0f }, 20.0f }, { { 0.0f, 0.0f, 1.0f }, 20.0f },
        { { 0.0f, 1.0f, 0.0f }, 10.0f }, { { 0.0f, -1.0f, 0.0f }, 10.0f },
    };
    CollisionWorld world;
    world.AddConvex({}, planes, { -20.0f, -10.0f, -20.0f }, { 20.0f, 10.0f, 20.0f });

    const CollisionHit hit = world.SweepCapsule({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 10.0f }, 0.5f, 0.5f);
    REQUIRE(hit.hit);
    CHECK_NEAR(hit.normal.x, -d, 1e-4);
    CHECK_NEAR(hit.normal.z, -d, 1e-4);
    CHECK(hit.fraction > 0.5f && hit.fraction < 0.7f);

    CHECK(!world.SweepCapsule({ 0.0f, 0.0f, 0.0f }, { -3.0f, 0.0f, 0.0f }, 0.5f, 0.5f).hit);
}

TEST_CASE("collision: ray hits a sphere")
{
    CollisionWorld world;
    world.AddSphere({ 0.0f, 0.0f, 5.0f }, 2.0f);

    RaycastHit hit;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit));
    CHECK_NEAR(hit.distance, 3.0, 1e-4);
}

TEST_CASE("collision: ray respects distance, ignore and triggers")
{
    CollisionWorld world;
    const ColliderHandle solid = world.AddBox({ 0.0f, 0.0f, 5.0f }, { 1.0f, 1.0f, 1.0f });
    const ColliderHandle trigger = world.AddBox({ 0.0f, 0.0f, 2.0f }, { 0.5f, 0.5f, 0.5f }, true);

    RaycastHit hit;
    RaycastQuery query;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit, query));
    CHECK(hit.other == solid);

    query.includeTriggers = true;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit, query));
    CHECK(hit.other == trigger);

    query.ignore = trigger;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit, query));
    CHECK(hit.other == solid);

    RaycastQuery shortQuery;
    shortQuery.maxDistance = 1.0f;
    CHECK(!world.Raycast(RayAlongZ(0.0f), hit, shortQuery));
}

TEST_CASE("collision: raycast all sorts hits by distance")
{
    CollisionWorld world;
    const ColliderHandle farBox = world.AddBox({ 0.0f, 0.0f, 9.0f }, { 1.0f, 1.0f, 1.0f });
    const ColliderHandle nearBox = world.AddBox({ 0.0f, 0.0f, 3.0f }, { 1.0f, 1.0f, 1.0f });

    std::vector<RaycastHit> hits;
    REQUIRE(world.RaycastAll(RayAlongZ(0.0f), hits) == 2);
    CHECK(hits[0].other == nearBox);
    CHECK(hits[1].other == farBox);
}

TEST_CASE("collision: overlap checks")
{
    CollisionWorld world;
    const ColliderHandle box = world.AddBox({}, { 1.0f, 1.0f, 1.0f });

    CHECK(world.CheckBox({ 1.5f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }));
    CHECK(!world.CheckBox({ 3.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }));
    CHECK(world.CheckSphere({ 1.5f, 0.0f, 0.0f }, 1.0f));
    CHECK(!world.CheckSphere({ 3.0f, 0.0f, 0.0f }, 1.0f));
    CHECK(world.CheckCapsule({ 0.0f, 2.0f, 0.0f }, 0.5f, 1.0f));
    CHECK(!world.CheckSphere({ 1.5f, 0.0f, 0.0f }, 1.0f, box));
}

TEST_CASE("collision: capsule sweep stops at walls")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, 0.0f, 5.0f }, { 2.0f, 2.0f, 0.5f });

    CHECK(world.SweepCapsule({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 10.0f }, 0.5f, 0.5f).hit);
    CHECK(!world.SweepCapsule({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 2.0f }, 0.5f, 0.5f).hit);
}

TEST_CASE("collision: convex hull blocks rays and overlaps")
{
    const std::vector<CollisionPlane> planes = {
        { { 1.0f, 0.0f, 0.0f }, 1.0f }, { { -1.0f, 0.0f, 0.0f }, 1.0f },
        { { 0.0f, 1.0f, 0.0f }, 1.0f }, { { 0.0f, -1.0f, 0.0f }, 1.0f },
        { { 0.0f, 0.0f, 1.0f }, 1.0f }, { { 0.0f, 0.0f, -1.0f }, 1.0f },
    };

    CollisionWorld world;
    world.AddConvex({ 0.0f, 0.0f, 5.0f }, planes, { -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });

    RaycastHit hit;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit));
    CHECK_NEAR(hit.distance, 4.0, 1e-4);
    CHECK_NEAR(hit.normal.z, -1.0, 1e-4);
    CHECK(world.CheckSphere({ 0.0f, 0.0f, 5.0f }, 0.5f));
    CHECK(!world.CheckSphere({ 0.0f, 0.0f, 0.0f }, 0.5f));
}

TEST_CASE("collision: bodies move, disable and get removed")
{
    CollisionWorld world;
    const ColliderHandle box = world.AddBox({}, { 1.0f, 1.0f, 1.0f });
    CHECK(world.Exists(box));

    world.SetPosition(box, { 10.0f, 0.0f, 0.0f });
    CHECK_NEAR(world.GetWorldPosition(box).x, 10.0, 1e-6);
    CHECK(!world.CheckSphere({}, 0.5f));
    CHECK(world.CheckSphere({ 10.0f, 0.0f, 0.0f }, 0.5f));

    world.SetActive(box, false);
    CHECK(!world.CheckSphere({ 10.0f, 0.0f, 0.0f }, 0.5f));
    world.SetActive(box, true);
    CHECK(world.CheckSphere({ 10.0f, 0.0f, 0.0f }, 0.5f));

    CHECK(world.Remove(box));
    CHECK(!world.Remove(box));
    CHECK(!world.Exists(box));
}

TEST_CASE("collision: a ray starting inside a shape hits it at once")
{
    CollisionWorld world;
    const ColliderHandle box = world.AddBox({}, { 1.0f, 1.0f, 1.0f });

    RaycastHit hit;
    REQUIRE(world.Raycast(RayAlongZ(0.0f), hit));
    CHECK(hit.other == box);
    CHECK(hit.distance == 0.0f);

    std::vector<RaycastHit> hits;
    CHECK(world.RaycastAll(RayAlongZ(0.0f), hits) == 1);
}

TEST_CASE("physics: a dynamic box falls and rests on the floor")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, -1.0f, 0.0f }, { 10.0f, 1.0f, 10.0f });
    RigidBodyDesc desc;
    desc.position = { 0.0f, 3.0f, 0.0f };
    const ColliderHandle box = world.AddRigidBox({ 0.5f, 0.5f, 0.5f }, {}, desc);

    for (int step = 0; step < 200; ++step)
        world.Step(1.0f / 50.0f);

    Vector3 position, rotation;
    REQUIRE(world.GetBodyPose(box, 1.0f, position, rotation));
    CHECK_NEAR(position.y, 0.5, 0.02);
    CHECK_NEAR(position.x, 0.0, 0.01);
    CHECK_NEAR(world.GetLinearVelocity(box).y, 0.0, 0.05);
}

TEST_CASE("physics: a rolling sphere comes to rest")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, -1.0f, 0.0f }, { 100.0f, 1.0f, 100.0f });
    RigidBodyDesc desc;
    desc.position = { 0.0f, 0.5f, 0.0f };
    const ColliderHandle ball = world.AddRigidSphere(0.5f, {}, desc);
    world.SetLinearVelocity(ball, { 2.0f, 0.0f, 0.0f });

    for (int step = 0; step < 500; ++step)
        world.Step(1.0f / 50.0f);

    CHECK_NEAR(world.GetLinearVelocity(ball).x, 0.0, 0.01);
    CHECK(world.GetWorldPosition(ball).x < 50.0f);
}

TEST_CASE("physics: rotations convert in the entity convention")
{
    CollisionWorld world;
    RigidBodyDesc desc;
    desc.motion = ColliderMotion::Kinematic;
    desc.rotation = { 0.4f, -1.1f, 0.7f };
    const ColliderHandle box = world.AddRigidBox({ 1.0f, 1.0f, 1.0f }, {}, desc);

    Vector3 position, rotation;
    REQUIRE(world.GetBodyPose(box, 1.0f, position, rotation));
    CHECK_NEAR(rotation.x, 0.4, 1e-4);
    CHECK_NEAR(rotation.y, -1.1, 1e-4);
    CHECK_NEAR(rotation.z, 0.7, 1e-4);

    // Yaw alone turns local +Z towards +X, like the camera: a thin wall along
    // local Z ends up along X, so a ray down Z only meets its thickness.
    desc.rotation = { 0.0f, 1.57079633f, 0.0f };
    desc.position = { 0.0f, 0.0f, 20.0f };
    world.AddRigidBox({ 0.1f, 1.0f, 5.0f }, {}, desc);
    RaycastHit hit;
    RaycastQuery query;
    query.ignore = box;
    REQUIRE(world.Raycast({ { 0.0f, 0.0f, 10.0f }, { 0.0f, 0.0f, 1.0f } }, hit, query));
    CHECK_NEAR(hit.distance, 9.9, 1e-3);
}

TEST_CASE("physics: pose interpolates between the last two steps")
{
    CollisionWorld world;
    RigidBodyDesc desc;
    desc.position = { 0.0f, 100.0f, 0.0f };
    const ColliderHandle box = world.AddRigidBox({ 0.5f, 0.5f, 0.5f }, {}, desc);
    world.Step(1.0f / 50.0f);
    world.Step(1.0f / 50.0f);

    Vector3 previous, middle, latest, rotation;
    REQUIRE(world.GetBodyPose(box, 0.0f, previous, rotation));
    REQUIRE(world.GetBodyPose(box, 0.5f, middle, rotation));
    REQUIRE(world.GetBodyPose(box, 1.0f, latest, rotation));
    CHECK(latest.y < previous.y);
    CHECK_NEAR(middle.y, (previous.y + latest.y) * 0.5, 1e-4);
}

TEST_CASE("physics: a kinematic capsule pushes a dynamic box")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, -1.0f, 0.0f }, { 50.0f, 1.0f, 50.0f });
    RigidBodyDesc desc;
    desc.position = { 0.0f, 0.5f, 2.0f };
    const ColliderHandle box = world.AddRigidBox({ 0.5f, 0.5f, 0.5f }, {}, desc);
    Vector3 player = { 0.0f, 1.0f, 0.0f };
    const ColliderHandle capsule = world.AddCapsule(player, 0.35f, 0.6f, false, {}, ColliderMotion::Kinematic);

    for (int step = 0; step < 100; ++step)
    {
        player = world.MoveCapsule(player, { 0.0f, 0.0f, 0.08f }, 0.35f, 0.6f, capsule);
        world.SetTargetPosition(capsule, player, 1.0f / 50.0f);
        world.Step(1.0f / 50.0f);
    }

    Vector3 position, rotation;
    REQUIRE(world.GetBodyPose(box, 1.0f, position, rotation));
    CHECK(position.z > 3.0f);
    CHECK(player.z > 3.0f);
}

TEST_CASE("mover: stops at a wall and slides along an angled one")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, 0.0f, 5.0f }, { 20.0f, 20.0f, 1.0f });

    Vector3 position = {};
    for (int step = 0; step < 60; ++step)
        position = world.MoveCapsule(position, { 0.0f, 0.0f, 0.2f }, 0.5f, 0.5f);
    CHECK_NEAR(position.z, 3.5, 0.03);

    world.Clear();
    const float d = 0.70710678f;
    const std::vector<CollisionPlane> planes = {
        { { -d, 0.0f, -d }, -5.0f },
        { { 1.0f, 0.0f, 0.0f }, 40.0f }, { { 0.0f, 0.0f, 1.0f }, 40.0f },
        { { 0.0f, 1.0f, 0.0f }, 10.0f }, { { 0.0f, -1.0f, 0.0f }, 10.0f },
    };
    world.AddConvex({}, planes, { -40.0f, -10.0f, -40.0f }, { 40.0f, 10.0f, 40.0f });

    position = {};
    for (int step = 0; step < 100; ++step)
        position = world.MoveCapsule(position, { 0.0f, 0.0f, 0.2f }, 0.5f, 0.5f);
    CHECK(position.z > 5.0f);    // reached the slope and kept going along it
    CHECK(position.x < -3.0f);
    CHECK_NEAR(position.y, 0.0, 0.01);
}

TEST_CASE("mover: a capsule started inside a wall is pushed out")
{
    CollisionWorld world;
    world.AddBox({ 0.0f, 0.0f, 0.5f }, { 20.0f, 20.0f, 0.5f });

    Vector3 position = { 0.0f, 0.0f, -0.2f };
    for (int step = 0; step < 10; ++step)
        position = world.MoveCapsule(position, {}, 0.5f, 0.5f);
    CHECK(!world.CheckCapsule(position, 0.49f, 0.5f));
}

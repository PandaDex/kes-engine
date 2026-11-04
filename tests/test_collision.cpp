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
    CHECK(world.Find(box) != nullptr);

    world.SetPosition(box, { 10.0f, 0.0f, 0.0f });
    CHECK_NEAR(world.GetWorldPosition(box).x, 10.0, 1e-6);
    CHECK(!world.CheckSphere({}, 0.5f));
    CHECK(world.CheckSphere({ 10.0f, 0.0f, 0.0f }, 0.5f));

    world.Find(box)->active = false;
    CHECK(!world.CheckSphere({ 10.0f, 0.0f, 0.0f }, 0.5f));

    CHECK(world.Remove(box));
    CHECK(!world.Remove(box));
    CHECK(world.Find(box) == nullptr);
}

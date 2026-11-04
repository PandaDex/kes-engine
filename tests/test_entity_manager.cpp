#include "test_framework.h"
#include "entity_manager.h"

namespace
{
    struct Counters
    {
        int started = 0;
        int updated = 0;
        int destroyed = 0;
    };

    class CountingEntity : public Entity
    {
    public:
        explicit CountingEntity(Counters& counters) : m_counters(counters) {}

        void Start() override { ++m_counters.started; }
        void Update(float) override { ++m_counters.updated; }

    protected:
        void OnDestroy() override { ++m_counters.destroyed; }

    private:
        Counters& m_counters;
    };

    class SpawningEntity : public Entity
    {
    public:
        SpawningEntity(EntityManager& manager, Counters& counters, int spawnCount)
            : m_manager(manager), m_counters(counters), m_spawnCount(spawnCount) {}

        void Update(float) override
        {
            for (int i = 0; i < m_spawnCount; ++i)
                m_manager.CreateEntity<CountingEntity>(m_counters);
            m_spawnCount = 0;
        }

    private:
        EntityManager& m_manager;
        Counters& m_counters;
        int m_spawnCount;
    };

    class DestroyingEntity : public Entity
    {
    public:
        explicit DestroyingEntity(Entity** target) : m_target(target) {}
        void Update(float) override { (*m_target)->Destroy(); }

    private:
        Entity** m_target;
    };
}

TEST_CASE("entities: start once, then update every frame")
{
    EntityManager manager;
    Counters counters;
    manager.CreateEntity<CountingEntity>(counters);

    manager.Update(0.016f);
    manager.Update(0.016f);

    CHECK(counters.started == 1);
    CHECK(counters.updated == 2);
}

TEST_CASE("entities: spawning during update is safe")
{
    EntityManager manager;
    Counters counters;
    manager.CreateEntity<SpawningEntity>(manager, counters, 256);
    manager.CreateEntity<CountingEntity>(counters);

    manager.Update(0.016f);
    CHECK(counters.updated == 1);

    manager.Update(0.016f);
    CHECK(counters.started == 257);
    CHECK(counters.updated == 258);
}

TEST_CASE("entities: destroyed entities stop updating and run OnDestroy once")
{
    EntityManager manager;
    Counters counters;
    CountingEntity* entity = manager.CreateEntity<CountingEntity>(counters);
    const uint64_t id = entity->GetEntityId();
    manager.Update(0.016f);

    entity->Destroy();
    CHECK(manager.GetEntityById(id) == nullptr);
    manager.Update(0.016f);
    CHECK(counters.updated == 1);

    manager.DestroyPending();
    manager.DestroyPending();
    CHECK(counters.destroyed == 1);
}

TEST_CASE("entities: destroying a later entity during update skips it")
{
    EntityManager manager;
    Counters counters;
    Entity* victim = nullptr;
    manager.CreateEntity<DestroyingEntity>(&victim);
    victim = manager.CreateEntity<CountingEntity>(counters);

    manager.Update(0.016f);
    CHECK(counters.started == 1);
    CHECK(counters.updated == 0);
}

TEST_CASE("entities: ids stay unique after destroying everything")
{
    EntityManager manager;
    Counters counters;
    const uint64_t first = manager.CreateEntity<CountingEntity>(counters)->GetEntityId();
    manager.DestroyAll();
    CHECK(counters.destroyed == 1);

    const uint64_t second = manager.CreateEntity<CountingEntity>(counters)->GetEntityId();
    CHECK(first != second);
}

TEST_CASE("entities: lookups find classes and tags")
{
    EntityManager manager;
    Counters counters;
    CountingEntity* plain = manager.CreateEntity<CountingEntity>(counters);
    CountingEntity* tagged = manager.CreateEntity<CountingEntity>(counters);
    tagged->Tags.push_back("boss");

    CHECK(manager.GetEntityOfClass<CountingEntity>() == plain);
    CHECK(manager.GetEntityOfClassWithTag<CountingEntity>("boss") == tagged);
    CHECK(manager.GetEntityOfClassWithTag<CountingEntity>("missing") == nullptr);
    CHECK(manager.GetEntityOfClass<SpawningEntity>() == nullptr);
    CHECK(manager.GetEntityById(tagged->GetEntityId()) == tagged);
}

TEST_CASE("entities: gravity is opt in")
{
    EntityManager manager;
    Counters counters;
    CountingEntity* floating = manager.CreateEntity<CountingEntity>(counters);
    CountingEntity* falling = manager.CreateEntity<CountingEntity>(counters);
    CHECK(falling->ApplyProperty("UseGravity", "true"));

    manager.FixedUpdate(0.1f);

    CHECK_NEAR(floating->Position.y, 0.0, 1e-6);
    CHECK(falling->Position.y < 0.0f);
}

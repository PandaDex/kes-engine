#include "test_framework.h"
#include "map_file.h"

namespace
{
    struct Writer
    {
        std::vector<uint8_t> data;

        void Raw(const void* bytes, size_t size)
        {
            const uint8_t* begin = static_cast<const uint8_t*>(bytes);
            data.insert(data.end(), begin, begin + size);
        }
        void U32(uint32_t value) { Raw(&value, sizeof(value)); }
        void I32(int32_t value) { Raw(&value, sizeof(value)); }
        void F32(float value) { Raw(&value, sizeof(value)); }
        void U8(uint8_t value) { Raw(&value, sizeof(value)); }
        void Vec3(float x, float y, float z)
        {
            F32(x);
            F32(y);
            F32(z);
        }
        void Str(const std::string& value)
        {
            U32(static_cast<uint32_t>(value.size()));
            Raw(value.data(), value.size());
        }
        void Chunk(const char* id, const Writer& chunk)
        {
            Raw(id, 4);
            U32(static_cast<uint32_t>(chunk.data.size()));
            Raw(chunk.data.data(), chunk.data.size());
        }
    };

    Writer MakeHeader()
    {
        Writer map;
        map.Raw("FORGECMP", 8);
        map.U32(2);
        map.U32(0);
        return map;
    }

    Writer MakeMaterials()
    {
        Writer chunk;
        chunk.U32(1);
        chunk.Str("materials/floor.kmat");
        return chunk;
    }

    Writer MakeGeometry(uint32_t material)
    {
        Writer chunk;
        chunk.U32(1);
        chunk.I32(-1);
        chunk.U32(material);
        chunk.U32(3);
        chunk.U32(3);
        for (int i = 0; i < 3; ++i)
        {
            chunk.Vec3(static_cast<float>(i), 0.0f, 0.0f);
            chunk.Vec3(0.0f, 0.0f, 1.0f);
            chunk.F32(0.0f);
            chunk.F32(1.0f);
        }
        for (uint32_t i = 0; i < 3; ++i)
            chunk.U32(i);
        return chunk;
    }

    Writer MakeEntities()
    {
        Writer chunk;
        chunk.U32(1);
        chunk.Str("trigger");
        chunk.Str("door_trigger");
        chunk.U8(1);
        chunk.Vec3(1.0f, 2.0f, 3.0f);
        chunk.Vec3(0.0f, 90.0f, 0.0f);
        chunk.Vec3(-1.0f, -1.0f, -1.0f);
        chunk.Vec3(1.0f, 1.0f, 1.0f);
        chunk.U32(1);
        chunk.Str("Tag");
        chunk.Str("door");
        chunk.U32(1);
        chunk.Str("OnEnter");
        chunk.Str("door");
        chunk.Str("Open");
        chunk.Str("");
        chunk.F32(0.5f);
        chunk.U8(1);
        return chunk;
    }

    Writer MakeBrushes()
    {
        Writer chunk;
        chunk.U32(1);
        chunk.I32(0);
        chunk.Vec3(-1.0f, -1.0f, -1.0f);
        chunk.Vec3(1.0f, 1.0f, 1.0f);
        chunk.U32(1);
        chunk.Vec3(0.0f, 0.0f, 1.0f);
        chunk.F32(1.0f);
        return chunk;
    }

    Writer MakeMap()
    {
        Writer map = MakeHeader();
        map.Chunk("MATL", MakeMaterials());
        map.Chunk("GEOM", MakeGeometry(0));
        map.Chunk("ENTS", MakeEntities());
        map.Chunk("BRSH", MakeBrushes());
        return map;
    }

    bool Load(const kestest::TempDir& dir, const Writer& map, CompiledMap& out)
    {
        const std::string path = dir.File("test.fmapc");
        kestest::WriteBytes(path, map.data);
        return LoadCompiledMap(path.c_str(), out);
    }
}

TEST_CASE("map: loads every chunk")
{
    kestest::TempDir dir;
    CompiledMap map;
    REQUIRE(Load(dir, MakeMap(), map));

    REQUIRE(map.materials.size() == 1);
    CHECK(map.materials[0] == "materials/floor.kmat");

    REQUIRE(map.meshes.size() == 1);
    CHECK(map.meshes[0].entity == -1);
    CHECK(map.meshes[0].vertices.size() == 3);
    CHECK(map.meshes[0].indices.size() == 3);

    REQUIRE(map.entities.size() == 1);
    const MapEntity& entity = map.entities[0];
    CHECK(entity.className == "trigger");
    CHECK(entity.solid);
    CHECK_NEAR(entity.origin.z, 3.0, 1e-6);
    REQUIRE(entity.keyValues.size() == 1);
    CHECK(entity.keyValues[0].second == "door");
    REQUIRE(entity.outputs.size() == 1);
    CHECK(entity.outputs[0].input == "Open");
    CHECK(entity.outputs[0].once);

    REQUIRE(map.brushes.size() == 1);
    REQUIRE(map.brushes[0].planes.size() == 1);
    CHECK_NEAR(map.brushes[0].planes[0].dist, 1.0, 1e-6);
}

TEST_CASE("map: skips unknown chunks")
{
    kestest::TempDir dir;
    Writer map = MakeMap();
    Writer unknown;
    unknown.U32(1234);
    map.Chunk("NEW!", unknown);

    CompiledMap loaded;
    CHECK(Load(dir, map, loaded));
    CHECK(loaded.materials.size() == 1);
}

TEST_CASE("map: rejects bad headers")
{
    kestest::TempDir dir;
    CompiledMap loaded;

    Writer badMagic = MakeMap();
    badMagic.data[0] = 'X';
    CHECK(!Load(dir, badMagic, loaded));

    Writer badVersion = MakeMap();
    badVersion.data[8] = 99;
    CHECK(!Load(dir, badVersion, loaded));
}

TEST_CASE("map: rejects a chunk cut short")
{
    kestest::TempDir dir;
    Writer map = MakeMap();
    map.data.resize(map.data.size() - 1);

    CompiledMap loaded;
    CHECK(!Load(dir, map, loaded));
}

TEST_CASE("map: rejects counts larger than the chunk")
{
    kestest::TempDir dir;
    Writer chunk;
    chunk.U32(0xFFFFFFFFu);
    Writer map = MakeHeader();
    map.Chunk("MATL", chunk);

    CompiledMap loaded;
    CHECK(!Load(dir, map, loaded));
}

TEST_CASE("map: rejects a mesh with a missing material")
{
    kestest::TempDir dir;
    Writer map = MakeHeader();
    map.Chunk("MATL", MakeMaterials());
    map.Chunk("GEOM", MakeGeometry(5));

    CompiledMap loaded;
    CHECK(!Load(dir, map, loaded));
}

TEST_CASE("map: converts editor space to engine space")
{
    const float scale = GetMapUnitScale();
    const Vector3 position = ConvertMapPosition({ 1.0f, 2.0f, 3.0f });
    CHECK_NEAR(position.x, 1.0f * scale, 1e-5);
    CHECK_NEAR(position.y, 3.0f * scale, 1e-5);
    CHECK_NEAR(position.z, 2.0f * scale, 1e-5);

    const Vector3 extents = ConvertMapExtents({ -1.0f, 2.0f, -3.0f });
    CHECK(extents.x > 0.0f);
    CHECK(extents.z > 0.0f);
}

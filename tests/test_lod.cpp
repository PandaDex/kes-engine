#include "test_framework.h"
#include <cmath>
#include "kmodel.h"
#include "render_common.h"
#include "scene.h"

namespace
{
    // A wavy grid (so simplification has some error to weigh), split into two material primitives.
    KModelMesh MakeGrid(const char* name, uint32_t cells, float offsetX = 0.0f)
    {
        KModelMesh mesh;
        mesh.name = name;
        for (uint32_t y = 0; y <= cells; ++y)
        {
            for (uint32_t x = 0; x <= cells; ++x)
            {
                KModelVertex vertex;
                vertex.px = offsetX + (float)x / (float)cells;
                vertex.pz = (float)y / (float)cells;
                vertex.py = 0.05f * std::sin(vertex.px * 6.0f) * std::cos(vertex.pz * 4.0f);
                vertex.u = vertex.px;
                vertex.v = vertex.pz;
                mesh.vertices.push_back(vertex);
            }
        }
        const uint32_t row = cells + 1;
        for (uint32_t y = 0; y < cells; ++y)
        {
            for (uint32_t x = 0; x < cells; ++x)
            {
                const uint32_t i = y * row + x;
                mesh.indices.insert(mesh.indices.end(), { i, i + row, i + 1, i + 1, i + row, i + row + 1 });
            }
        }
        const uint32_t half = (uint32_t)mesh.indices.size() / 6 * 3;
        mesh.primitives.push_back({ 0, half, 0 });
        mesh.primitives.push_back({ half, (uint32_t)mesh.indices.size() - half, 1 });
        return mesh;
    }

    KModelData MakeGridModel()
    {
        KModelData model;
        model.materials.push_back({ "a", "" });
        model.materials.push_back({ "b", "" });
        model.meshes.push_back(MakeGrid("grid", 32));
        model.nodes.resize(1);
        model.nodes[0].meshIndex = 0;
        return model;
    }

    uint32_t TriangleCount(const KModelMeshLod& lod)
    {
        return (uint32_t)lod.indices.size() / 3;
    }
}

TEST_CASE("lod: generated levels shrink, keep slots and reference the base vertices")
{
    KModelData model = MakeGridModel();
    SetDefaultKModelLodLevels(model, 3);
    model.autoLods = true;
    REQUIRE(GenerateKModelLods(model));

    const KModelMesh& mesh = model.meshes[0];
    REQUIRE(mesh.lods.size() == 3);
    uint32_t previous = (uint32_t)mesh.indices.size() / 3;
    for (const KModelMeshLod& lod : mesh.lods)
    {
        CHECK(TriangleCount(lod) < previous);
        CHECK(TriangleCount(lod) > 0);
        previous = TriangleCount(lod);
        REQUIRE(lod.primitives.size() == 2);
        CHECK(lod.primitives[0].materialIndex == 0);
        CHECK(lod.primitives[1].materialIndex == 1);
        CHECK(lod.primitives[0].firstIndex + lod.primitives[0].indexCount == lod.primitives[1].firstIndex);
        for (uint32_t index : lod.indices)
            CHECK(index < mesh.vertices.size());
    }
    // The first level aims at half of LOD 0.
    CHECK(TriangleCount(mesh.lods[0]) <= mesh.indices.size() / 3 / 2 + 8);
}

TEST_CASE("lod: levels and mesh LODs survive a save and load")
{
    kestest::TempDir dir;
    const std::string path = dir.File("lods.kmodel");
    KModelData model = MakeGridModel();
    SetDefaultKModelLodLevels(model, 2);
    model.lods[1].screenSize = 0.1f;
    model.autoLods = true;
    REQUIRE(GenerateKModelLods(model));
    REQUIRE(SaveKModelFile(path.c_str(), model));

    KModelData loaded;
    std::string error;
    REQUIRE(LoadKModelFile(path.c_str(), loaded, &error));
    CHECK(loaded.autoLods);
    REQUIRE(loaded.lods.size() == 2);
    CHECK(loaded.lods[1].screenSize == 0.1f);
    CHECK(loaded.lods[0].triangleRatio == 0.5f);
    REQUIRE(loaded.meshes[0].lods.size() == 2);
    CHECK(loaded.meshes[0].lods[1].indices == model.meshes[0].lods[1].indices);
    CHECK(loaded.meshes[0].lods[0].primitives.size() == 2);

    // A mesh LOD count that doesn't match the levels is rejected.
    loaded.meshes[0].lods.pop_back();
    REQUIRE(SaveKModelFile(path.c_str(), loaded));
    KModelData broken;
    CHECK(!(LoadKModelFile(path.c_str(), broken)));
}

TEST_CASE("lod: mesh data puts LOD triangles after the base indices")
{
    KModelData model = MakeGridModel();
    model.meshes.push_back(MakeGrid("plain", 2, 2.0f)); // no LODs of its own
    model.meshes[1].primitives.clear();
    model.nodes.resize(2);
    model.nodes[1].meshIndex = 1;
    SetDefaultKModelLodLevels(model, 2);
    REQUIRE(GenerateKModelLods(model));
    model.meshes[1].lods.clear();

    MeshData mesh;
    REQUIRE(BuildMeshDataFromKModel(model, mesh));
    REQUIRE(mesh.lods.size() == 2);
    CHECK(mesh.lods[0].screenSize == 0.5f);
    const size_t baseCount = mesh.indices.size();
    CHECK(mesh.lodIndices.size() == model.meshes[0].lods[0].indices.size() + model.meshes[0].lods[1].indices.size());
    const size_t gridBaseVertices = model.meshes[0].vertices.size();
    for (const MeshLod& lod : mesh.lods)
    {
        bool plainSection = false;
        for (const MeshSection& section : lod.sections)
        {
            if (section.firstIndex >= baseCount)
                CHECK(section.firstIndex + section.indexCount <= baseCount + mesh.lodIndices.size());
            else
            {
                // The second mesh draws its LOD 0 indices, right after the grid's.
                plainSection = true;
                CHECK(section.firstIndex == model.meshes[0].indices.size());
                CHECK(section.materialSlot == -1);
            }
        }
        CHECK(plainSection);
    }
    for (uint32_t index : mesh.lodIndices)
        CHECK(index < gridBaseVertices);
}

TEST_CASE("lod: selection by screen size, bias and forced level")
{
    std::vector<MeshLod> lods(2);
    lods[0].screenSize = 0.5f;
    lods[1].screenSize = 0.25f;
    SceneObject object;
    GraphicsOptions options;

    CHECK(render::SelectLod(lods, 0.8f, object, options) == 0);
    CHECK(render::SelectLod(lods, 0.4f, object, options) == 1);
    CHECK(render::SelectLod(lods, 0.1f, object, options) == 2);
    CHECK(render::SelectLod({}, 0.1f, object, options) == 0);

    options.lodBias = 2.0f;
    CHECK(render::SelectLod(lods, 0.4f, object, options) == 0);
    options.lodBias = 1.0f;

    options.forcedLod = 1;
    CHECK(render::SelectLod(lods, 0.9f, object, options) == 1);
    object.forcedLod = 5;
    CHECK(render::SelectLod(lods, 0.9f, object, options) == 2);

    // A 1 m radius sphere 10 m away with a 90 degree vertical fov covers a tenth of the screen height.
    const float size = render::ProjectedScreenSize(Vector3(0.0f, 0.0f, 10.0f), 1.0f, Vector3(0.0f, 0.0f, 0.0f), 3.14159265f * 0.5f);
    CHECK(std::fabs(size - 0.1f) < 0.001f);
    CHECK(render::ProjectedScreenSize(Vector3(0.0f, 0.0f, 0.5f), 1.0f, Vector3(0.0f, 0.0f, 0.0f), 1.0f) == 1.0f);
}

TEST_CASE("lod: source meshes named _LOD<n> become LODs of their base mesh")
{
    KModelData model;
    model.materials.push_back({ "a", "" });
    model.materials.push_back({ "b", "" });
    model.meshes.push_back(MakeGrid("Rock_LOD0", 8));
    model.meshes.push_back(MakeGrid("other", 2));
    model.meshes.push_back(MakeGrid("mesh_data_name", 2)); // node "Rock_LOD2"
    model.meshes.push_back(MakeGrid("Rock.lod1", 4));
    model.nodes.resize(4);
    for (int32_t i = 0; i < 4; ++i)
        model.nodes[(size_t)i].meshIndex = i;
    model.nodes[2].name = "Rock_LOD2";
    model.autoLods = true;

    CHECK(ExtractKModelSourceLods(model) == 2);
    CHECK(!(model.autoLods));
    REQUIRE(model.meshes.size() == 2);
    REQUIRE(model.lods.size() == 2);
    const KModelMesh& rock = model.meshes[0];
    CHECK(rock.name == "Rock_LOD0");
    CHECK(rock.vertices.size() == 81 + 25 + 9);
    REQUIRE(rock.lods.size() == 2);
    CHECK(rock.lods[0].indices.size() == 4 * 4 * 6);
    CHECK(rock.lods[1].indices.size() == 2 * 2 * 6);
    // Appended in source order: the LOD2 mesh comes first.
    CHECK(rock.lods[0].indices[0] >= 81 + 9);
    CHECK(rock.lods[1].indices[0] >= 81);
    CHECK(rock.lods[1].indices[0] < 81 + 9);
    CHECK(model.meshes[1].lods.empty());
    CHECK(model.nodes[1].meshIndex == 1);
    CHECK(model.nodes[2].meshIndex == -1);
    CHECK(model.nodes[3].meshIndex == -1);

    MeshData mesh;
    REQUIRE(BuildMeshDataFromKModel(model, mesh));
    CHECK(mesh.lods.size() == 2);
}

TEST_CASE("lod: re-import keeps generated LOD settings and tuned screen sizes")
{
    kestest::TempDir dir;
    const std::string path = dir.File("model.kmodel");
    KModelData existing = MakeGridModel();
    SetDefaultKModelLodLevels(existing, 2);
    existing.lods[0].screenSize = 0.3f;
    existing.lods[1].triangleRatio = 0.1f;
    existing.autoLods = true;
    REQUIRE(GenerateKModelLods(existing));
    REQUIRE(SaveKModelFile(path.c_str(), existing));

    KModelData reimported = MakeGridModel();
    CarryOverKModelAttachments(path.c_str(), reimported);
    CHECK(reimported.autoLods);
    REQUIRE(reimported.lods.size() == 2);
    CHECK(reimported.lods[0].screenSize == 0.3f);
    CHECK(reimported.lods[1].triangleRatio == 0.1f);

    KModelData handMade = MakeGridModel();
    handMade.lods.resize(2);
    CarryOverKModelAttachments(path.c_str(), handMade);
    CHECK(!(handMade.autoLods));
    CHECK(handMade.lods[0].screenSize == 0.3f);
}

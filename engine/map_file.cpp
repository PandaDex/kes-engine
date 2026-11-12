#include "map_file.h"
#include "binary_reader.h"
#include <cmath>
#include <cstring>
#include "engine_configuration.h"
#include "pak_file_system.h"
#include "profiler.h"

namespace
{
    constexpr uint32_t kMapVersion = 3;
    constexpr float kDegToRad = 3.14159265358979f / 180.0f;

    Vector3 ReadVec3(BinaryReader& r)
    {
        const float x = r.f32();
        const float y = r.f32();
        const float z = r.f32();
        return { x, y, z };
    }

    Guid ReadGuid(BinaryReader& r)
    {
        uint8_t bytes[16] = {};
        r.bytes(bytes, sizeof(bytes));
        return Guid::FromBytes(bytes);
    }

    bool ReadMaterials(BinaryReader& r, CompiledMap& map)
    {
        const uint32_t count = r.u32();
        if (!r.count(count, 4))
            return false;
        map.materials.resize(count);
        for (std::string& material : map.materials)
            material = r.string();
        return r.ok();
    }

    bool ReadGeometry(BinaryReader& r, CompiledMap& map)
    {
        const uint32_t count = r.u32();
        if (!r.count(count, 16))
            return false;
        map.meshes.resize(count);
        for (MapMesh& mesh : map.meshes)
        {
            mesh.entity = r.i32();
            mesh.material = r.u32();
            const uint32_t vertexCount = r.u32();
            const uint32_t indexCount = r.u32();
            if (!r.count(vertexCount, 32))
                return false;
            mesh.vertices.resize(vertexCount);
            for (MapVertex& v : mesh.vertices)
            {
                v.position = ReadVec3(r);
                v.normal = ReadVec3(r);
                v.u = r.f32();
                v.v = r.f32();
            }
            if (!r.count(indexCount, 4))
                return false;
            mesh.indices.resize(indexCount);
            if (indexCount > 0)
                r.bytes(mesh.indices.data(), indexCount * sizeof(uint32_t));
            for (uint32_t index : mesh.indices)
            {
                if (index >= vertexCount)
                    return false;
            }
        }
        return r.ok();
    }

    bool ReadEntities(BinaryReader& r, CompiledMap& map)
    {
        const uint32_t count = r.u32();
        if (!r.count(count, 4))
            return false;
        map.entities.resize(count);
        for (MapEntity& e : map.entities)
        {
            e.className = r.string();
            e.name = r.string();
            e.guid = ReadGuid(r);
            e.solid = r.u8() != 0;
            e.origin = ReadVec3(r);
            e.angles = ReadVec3(r);
            e.mins = ReadVec3(r);
            e.maxs = ReadVec3(r);

            const uint32_t kvCount = r.u32();
            if (!r.count(kvCount, 8))
                return false;
            e.keyValues.resize(kvCount);
            for (auto& [key, value] : e.keyValues)
            {
                key = r.string();
                value = r.string();
            }

            const uint32_t outputCount = r.u32();
            if (!r.count(outputCount, 33))
                return false;
            e.outputs.resize(outputCount);
            for (MapConnection& c : e.outputs)
            {
                c.output = r.string();
                c.target = ReadGuid(r);
                c.input = r.string();
                c.param = r.string();
                c.delay = r.f32();
                c.once = r.u8() != 0;
            }
        }
        return r.ok();
    }

    bool ReadBrushes(BinaryReader& r, CompiledMap& map)
    {
        const uint32_t count = r.u32();
        if (!r.count(count, 32))
            return false;
        map.brushes.resize(count);
        for (MapBrush& brush : map.brushes)
        {
            brush.entity = r.i32();
            brush.mins = ReadVec3(r);
            brush.maxs = ReadVec3(r);
            const uint32_t planeCount = r.u32();
            if (!r.count(planeCount, 16))
                return false;
            brush.planes.resize(planeCount);
            for (MapPlane& plane : brush.planes)
            {
                plane.normal = ReadVec3(r);
                plane.dist = r.f32();
            }
        }
        return r.ok();
    }
}

bool LoadCompiledMap(const char* path, CompiledMap& outMap, std::string* outError)
{
    KES_ZONE_FN(ASSETS);
    KES_ZONE_TEXTF(ASSETS, "%s", path ? path : "<null>");
    outMap = {};
    auto fail = [&](const std::string& message)
    {
        if (outError)
            *outError = message;
        outMap = {};
        return false;
    };

    std::vector<uint8_t> bytes;
    std::string error;
    if (!LoadAssetFileBytes(path, bytes, &error))
        return fail(error.empty() ? "Failed to read map file" : error);

    BinaryReader r(bytes.data(), bytes.size());
    char magic[8] = {};
    r.bytes(magic, sizeof(magic));
    if (!r.ok() || std::memcmp(magic, "FORGECMP", 8) != 0)
        return fail("Not a Forge compiled map (bad magic)");

    const uint32_t version = r.u32();
    if (version != kMapVersion)
        return fail("Unsupported compiled map version " + std::to_string(version) +
                    " (expected " + std::to_string(kMapVersion) + "); rebuild the map with the current editor");
    r.u32(); // flags, reserved

    while (r.ok() && r.remaining() > 0)
    {
        char id[4] = {};
        r.bytes(id, sizeof(id));
        const uint32_t size = r.u32();
        if (!r.ok() || size > r.remaining())
            return fail("Truncated chunk in map file");

        BinaryReader chunk(bytes.data() + r.position(), size);
        bool chunkOk = true;
        if (std::memcmp(id, "MATL", 4) == 0)
            chunkOk = ReadMaterials(chunk, outMap);
        else if (std::memcmp(id, "GEOM", 4) == 0)
            chunkOk = ReadGeometry(chunk, outMap);
        else if (std::memcmp(id, "ENTS", 4) == 0)
            chunkOk = ReadEntities(chunk, outMap);
        else if (std::memcmp(id, "BRSH", 4) == 0)
            chunkOk = ReadBrushes(chunk, outMap);
        // Other chunks are skipped (newer editor features).

        if (!chunkOk)
            return fail(std::string("Corrupt '") + std::string(id, 4) + "' chunk in map file");
        r.skip(size);
    }

    if (!r.ok())
        return fail("Truncated map file");

    for (const MapMesh& mesh : outMap.meshes)
    {
        if (mesh.material >= outMap.materials.size() && !(outMap.materials.empty() && mesh.material == 0))
            return fail("Map mesh references a missing material");
    }
    return true;
}

float GetMapUnitScale()
{
    return ENGINE_MAP_UNIT_SCALE;
}

// Editor: X east, Y north, Z up (right-handed). Engine: X right, Y up, Z forward
// (left-handed). Swapping Y and Z converts between the two handedness conventions.
Vector3 ConvertMapDirection(const Vector3& d)
{
    return { d.x, d.z, d.y };
}

Vector3 ConvertMapPosition(const Vector3& p)
{
    const float s = GetMapUnitScale();
    return { p.x * s, p.z * s, p.y * s };
}

Vector3 ConvertMapExtents(const Vector3& e)
{
    const float s = GetMapUnitScale();
    return { std::fabs(e.x) * s, std::fabs(e.z) * s, std::fabs(e.y) * s };
}

Vector3 ConvertMapAngles(const Vector3& a)
{
    // Editor: yaw 0 = +X, counter-clockwise from above; positive pitch looks up.
    // Engine: yaw 0 = +Z, forward = (sin yaw cos pitch, -sin pitch, cos yaw cos pitch).
    return {
        -a.x * kDegToRad,
        (90.0f - a.y) * kDegToRad,
        a.z * kDegToRad
    };
}

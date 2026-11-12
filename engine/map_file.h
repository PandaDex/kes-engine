#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "engine_types.h"
#include "guid.h"

// Compiled map produced by the Forge editor (tools/forgeMapEditor, "Build Map" / forge --compile).
//
// File layout (.fmapc, version 3, little-endian):
//   char[8] "FORGECMP", u32 version (3), u32 flags (0)
//   chunks until end of file: char[4] id, u32 byteSize, byteSize bytes of data
//     "MATL"  u32 count, string[count]                      material paths ("" = engine default)
//     "GEOM"  u32 count, mesh[count]                        render geometry, one mesh per material
//             mesh: i32 entity, u32 material, u32 vertexCount, u32 indexCount,
//                   vertex { f32 px,py,pz, nx,ny,nz, u,v }[vertexCount], u32 index[indexCount]
//     "ENTS"  u32 count, entity[count]
//             entity: string class, string name, guid, u8 solid, f32x3 origin, f32x3 angles,
//                     f32x3 mins, f32x3 maxs, u32 kvCount, {string key, string value}[kvCount],
//                     u32 outputCount, {string output, guid target, string input,
//                                       string param, f32 delay, u8 once}[outputCount]
//     "BRSH"  u32 count, brush[count]                       convex collision hulls
//             brush: i32 entity, f32x3 mins, f32x3 maxs, u32 planeCount,
//                    {f32x3 normal, f32 dist}[planeCount]
//   Unknown chunks are skipped. Strings are u32 byte length + UTF-8. A guid is 16 bytes in
//   RFC 4122 order; output targets are the GUIDs of other entities (MapScript links).
//
// Everything is stored in editor space: Z-up, right-handed, editor units. World geometry is
// in world space; geometry and brushes owned by an entity (entity >= 0) are relative to its
// origin. ConvertMapPosition() & co. turn editor space into engine space (Y-up, left-handed,
// meters, scaled by ENGINE_MAP_UNIT_SCALE).

struct MapVertex
{
    Vector3 position;
    Vector3 normal;
    float u = 0.0f;
    float v = 0.0f;
};

struct MapMesh
{
    int32_t entity = -1;          // -1 = world
    uint32_t material = 0;        // index into CompiledMap::materials
    std::vector<MapVertex> vertices;
    std::vector<uint32_t> indices;
};

struct MapConnection
{
    std::string output;
    Guid target;
    std::string input;
    std::string param;
    float delay = 0.0f;
    bool once = false;
};

struct MapEntity
{
    std::string className;
    std::string name;
    Guid guid;            // assigned when the entity was placed in the editor
    bool solid = false;
    Vector3 origin;
    Vector3 angles;   // pitch, yaw, roll in degrees (editor convention)
    Vector3 mins;
    Vector3 maxs;
    std::vector<std::pair<std::string, std::string>> keyValues;
    std::vector<MapConnection> outputs;
};

struct MapPlane
{
    Vector3 normal;
    float dist = 0.0f;   // dot(normal, p) == dist on the plane, normal points out of the brush
};

struct MapBrush
{
    int32_t entity = -1;
    Vector3 mins;   // bounds of the hull (same space as the planes)
    Vector3 maxs;
    std::vector<MapPlane> planes;
};

struct CompiledMap
{
    std::vector<std::string> materials;
    std::vector<MapMesh> meshes;
    std::vector<MapEntity> entities;
    std::vector<MapBrush> brushes;
};

bool LoadCompiledMap(const char* path, CompiledMap& outMap, std::string* outError = nullptr);

// Editor space -> engine space.
float GetMapUnitScale();
Vector3 ConvertMapPosition(const Vector3& editorPosition);   // swizzled and scaled
Vector3 ConvertMapDirection(const Vector3& editorDirection); // swizzled only
Vector3 ConvertMapExtents(const Vector3& editorExtents);     // swizzled, scaled, positive
Vector3 ConvertMapAngles(const Vector3& editorAnglesDegrees); // -> engine Rotation (radians)

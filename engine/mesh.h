#pragma once
#include <cstdint>
#include <vector>

using MeshHandle = uint32_t;

struct MeshVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
};

// A run of indices drawn with one material slot (an index into KModelData::materials, -1 = no slot).
struct MeshSection
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t materialSlot = -1;
};

struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshSection> sections; // empty = one section over every index, slot -1
};

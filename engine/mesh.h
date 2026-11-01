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

struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

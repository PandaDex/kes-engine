#include "obj_importer.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

namespace
{
    struct Float2 { float x, y; };
    struct Float3 { float x, y, z; };

    struct ObjIndexKey
    {
        int v = 0;
        int vt = 0;
        int vn = 0;

        bool operator==(const ObjIndexKey& other) const
        {
            return v == other.v && vt == other.vt && vn == other.vn;
        }
    };

    struct ObjIndexKeyHasher
    {
        size_t operator()(const ObjIndexKey& key) const
        {
            size_t h = (size_t)key.v;
            h = (h * 73856093u) ^ (size_t)key.vt;
            h = (h * 19349663u) ^ (size_t)key.vn;
            return h;
        }
    };

    static int FixObjIndex(int index, int count)
    {
        if (index > 0)
            return index - 1;
        if (index < 0)
            return count + index;
        return -1;
    }

    static bool ParseFaceToken(const std::string& token, ObjIndexKey& outKey)
    {
        outKey = {};

        size_t first = token.find('/');
        if (first == std::string::npos)
        {
            outKey.v = std::stoi(token);
            return true;
        }

        size_t second = token.find('/', first + 1);
        std::string a = token.substr(0, first);
        std::string b = (second == std::string::npos) ? token.substr(first + 1) : token.substr(first + 1, second - first - 1);
        std::string c = (second == std::string::npos) ? std::string() : token.substr(second + 1);

        if (!a.empty()) outKey.v = std::stoi(a);
        if (!b.empty()) outKey.vt = std::stoi(b);
        if (!c.empty()) outKey.vn = std::stoi(c);
        return true;
    }
}

bool LoadObjMeshFromFile(const char* path, MeshData& outMesh)
{
    outMesh.vertices.clear();
    outMesh.indices.clear();

    std::ifstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    std::vector<Float3> positions;
    std::vector<Float3> normals;
    std::vector<Float2> uvs;
    std::unordered_map<ObjIndexKey, uint32_t, ObjIndexKeyHasher> vertexMap;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "v")
        {
            Float3 p = {};
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if (tag == "vn")
        {
            Float3 n = {};
            ss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (tag == "vt")
        {
            Float2 uv = {};
            ss >> uv.x >> uv.y;
            uvs.push_back(uv);
        }
        else if (tag == "f")
        {
            std::vector<uint32_t> faceIndices;
            std::string token;
            while (ss >> token)
            {
                ObjIndexKey key = {};
                if (!ParseFaceToken(token, key))
                    return false;

                // Absolute indices: a negative one means something else on every line.
                key.v = FixObjIndex(key.v, (int)positions.size());
                key.vt = FixObjIndex(key.vt, (int)uvs.size());
                key.vn = FixObjIndex(key.vn, (int)normals.size());

                auto it = vertexMap.find(key);
                if (it != vertexMap.end())
                {
                    faceIndices.push_back(it->second);
                    continue;
                }

                MeshVertex vertex = {};
                vertex.r = 1.0f;
                vertex.g = 1.0f;
                vertex.b = 1.0f;
                vertex.a = 1.0f;

                const int posIndex = key.v;
                const int uvIndex = key.vt;
                const int normalIndex = key.vn;

                if (posIndex < 0 || posIndex >= (int)positions.size())
                {
                    return false;
                }

                const Float3& p = positions[(size_t)posIndex];
                vertex.px = p.x;
                vertex.py = p.y;
                vertex.pz = p.z;

                if (normalIndex >= 0 && normalIndex < (int)normals.size())
                {
                    const Float3& n = normals[(size_t)normalIndex];
                    vertex.nx = n.x;
                    vertex.ny = n.y;
                    vertex.nz = n.z;
                }
                else
                {
                    vertex.nx = 0.0f;
                    vertex.ny = 1.0f;
                    vertex.nz = 0.0f;
                }

                if (uvIndex >= 0 && uvIndex < (int)uvs.size())
                {
                    const Float2& uv = uvs[(size_t)uvIndex];
                    vertex.u = uv.x;
                    vertex.v = 1.0f - uv.y;
                }
                else
                {
                    vertex.u = 0.0f;
                    vertex.v = 0.0f;
                }

                const uint32_t newIndex = (uint32_t)outMesh.vertices.size();
                outMesh.vertices.push_back(vertex);
                vertexMap[key] = newIndex;
                faceIndices.push_back(newIndex);
            }

            if (faceIndices.size() < 3)
                continue;

            for (size_t i = 1; i + 1 < faceIndices.size(); ++i)
            {
                outMesh.indices.push_back(faceIndices[0]);
                outMesh.indices.push_back(faceIndices[i]);
                outMesh.indices.push_back(faceIndices[i + 1]);
            }
        }
    }

    const bool valid = !outMesh.vertices.empty() && !outMesh.indices.empty();

    return valid;
}

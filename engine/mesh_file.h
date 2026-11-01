#pragma once
#include <string>
#include "mesh.h"

bool LoadMeshFromFile(const char* path, MeshData& outMesh, std::string* outError = nullptr);

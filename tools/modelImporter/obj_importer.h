#pragma once
#include <string>
#include <vector>
#include "mesh.h"

// usemtl names become material slots: outMesh.sections index into outMaterialNames.
bool LoadObjMeshFromFile(const char* path, MeshData& outMesh, std::vector<std::string>* outMaterialNames = nullptr);

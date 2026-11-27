#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "mesh.h"

using ModelHandle = uint32_t;
static constexpr uint32_t KModelAllMeshes = 0xFFFFFFFFu;

struct KModelVertex
{
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
    float tx = 1.0f;
    float ty = 0.0f;
    float tz = 0.0f;
    float tw = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    uint16_t boneIndices[4] = { 0, 0, 0, 0 };
    float boneWeights[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
};

struct KModelPrimitive
{
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t materialIndex = -1;
};

// A lower detail version of a mesh: its own triangles over the mesh's vertices,
// one primitive per primitive of the base mesh (same order and slots).
struct KModelMeshLod
{
    std::vector<uint32_t> indices;
    std::vector<KModelPrimitive> primitives;
};

struct KModelMesh
{
    std::string name;
    std::vector<KModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<KModelPrimitive> primitives;
    std::vector<KModelMeshLod> lods; // empty, or one per KModelData::lods
};

// LOD n (1-based) is drawn once the model's bounding sphere covers less than
// screenSize of the screen height. triangleRatio is the generation target (of LOD 0).
struct KModelLodLevel
{
    float screenSize = 0.5f;
    float triangleRatio = 0.5f;
};

// A material slot. Primitives pick one by index; an entity can override it by name ("Material.<name>").
struct KModelMaterial
{
    std::string name;
    std::string materialPath; // empty = the entity Material, then the model's default material
};

struct KModelNode
{
    std::string name;
    int32_t parentIndex = -1;
    int32_t meshIndex = -1;
    int32_t boneIndex = -1;
    float translation[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

struct KModelBone
{
    std::string name;
    int32_t nodeIndex = -1;
    int32_t parentBoneIndex = -1;
    float inverseBindMatrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
};

// Transform relative to a bone (boneIndex -1: the model root).
struct KModelSocket
{
    std::string name;
    int32_t boneIndex = -1;
    float translation[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

enum class KModelColliderShape : uint32_t
{
    Box = 0,
    Sphere = 1,
    Capsule = 2
};

// Placed relative to a bone (boneIndex -1: the model root) in the bind pose.
// Box uses halfExtents, Sphere radius, Capsule radius and halfHeight along its local Y.
struct KModelCollider
{
    std::string name;
    KModelColliderShape shape = KModelColliderShape::Box;
    int32_t boneIndex = -1;
    float center[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float halfExtents[3] = { 0.5f, 0.5f, 0.5f };
    float radius = 0.5f;
    float halfHeight = 0.5f;
};

// Another model attached to a bone of this one (boneIndex -1 = model root), like a gun in the hands.
struct KModelChild
{
    std::string name;
    std::string modelPath;
    std::string materialPath; // empty = use the child model's default material
    std::string animationPath;
    int32_t boneIndex = -1;
    float translation[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

struct KModelData
{
    std::string sourcePath;
    std::string defaultMaterial; // entity Material overrides it
    std::vector<KModelMaterial> materials;
    std::vector<KModelMesh> meshes;
    std::vector<KModelNode> nodes;
    std::vector<KModelBone> bones;
    std::vector<KModelSocket> sockets;
    std::vector<KModelCollider> colliders;
    std::vector<KModelChild> children;
    std::vector<KModelLodLevel> lods; // LOD 1..N, screen sizes decreasing
    bool autoLods = false; // the mesh LODs are generated from the levels (and again on re-import)
};

bool LoadKModelFile(const char* path, KModelData& outModel, std::string* outError = nullptr);
bool SaveKModelFile(const char* path, const KModelData& model, std::string* outError = nullptr);
// The draw sections of one mesh's primitives, offset by baseIndex; neighbours with the same slot merge.
void AppendKModelMeshSections(const KModelMesh& mesh, uint32_t baseIndex, std::vector<MeshSection>& sections);
bool BuildMeshDataFromKModel(const KModelData& model, MeshData& outMesh, std::string* outError = nullptr, uint32_t meshIndex = KModelAllMeshes);
// Re-import keeps hand-made sockets, colliders, children and the default material: copies them
// from the .kmodel at existingPath (if any), remapping bones by name. Returns how many were kept.
// Slot materials are kept by slot name. A primitive the new source gives no material keeps the slot
// it had (matched by mesh name and primitive order). LOD screen sizes are kept when the level count
// matches; autoLods levels are copied when the new model has none (mesh LODs are not regenerated).
size_t CarryOverKModelAttachments(const char* existingPath, KModelData& model);
const KModelSocket* FindKModelSocket(const KModelData& model, const char* name);
int32_t FindKModelMaterialSlot(const KModelData& model, const char* name); // -1 if missing
// Appends a mesh's LOD triangles (offset by baseVertex) to lodIndices and their sections to lods,
// resized to the model's level count. lodIndexOffset is where lodIndices start in the drawn index
// buffer; a mesh without LODs adds its LOD 0 sections (at baseIndex) to every level.
void AppendKModelMeshLods(const KModelData& model, const KModelMesh& mesh, uint32_t baseVertex, uint32_t baseIndex,
    uint32_t lodIndexOffset, std::vector<uint32_t>& lodIndices, std::vector<MeshLod>& lods);
// Simplifies every mesh to each level's triangleRatio (meshoptimizer). Replaces existing mesh LODs.
bool GenerateKModelLods(KModelData& model, std::string* outError = nullptr);
// Levels with the usual halving: ratio and screen size 1/2, 1/4, ...
void SetDefaultKModelLodLevels(KModelData& model, uint32_t count);
// Hand-made LODs from the source file: meshes named "<base>_LOD<n>" (n >= 1) become LODs of the mesh
// "<base>" (or "<base>_LOD0") and are removed. Their vertices are appended to the base mesh, so they
// must be in the same space. Sets autoLods to false. Returns how many meshes were merged.
uint32_t ExtractKModelSourceLods(KModelData& model);
void ConvertMeshDataToKModelMesh(const MeshData& mesh, KModelMesh& outMesh, const char* name = nullptr);

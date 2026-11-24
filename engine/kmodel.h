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

struct KModelMesh
{
    std::string name;
    std::vector<KModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<KModelPrimitive> primitives;
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
};

bool LoadKModelFile(const char* path, KModelData& outModel, std::string* outError = nullptr);
bool SaveKModelFile(const char* path, const KModelData& model, std::string* outError = nullptr);
// The draw sections of one mesh's primitives, offset by baseIndex; neighbours with the same slot merge.
void AppendKModelMeshSections(const KModelMesh& mesh, uint32_t baseIndex, std::vector<MeshSection>& sections);
bool BuildMeshDataFromKModel(const KModelData& model, MeshData& outMesh, std::string* outError = nullptr, uint32_t meshIndex = KModelAllMeshes);
// Re-import keeps hand-made sockets, colliders, children and the default material: copies them
// from the .kmodel at existingPath (if any), remapping bones by name. Returns how many were kept.
// Slot materials are kept by slot name. A primitive the new source gives no material keeps the slot
// it had (matched by mesh name and primitive order).
size_t CarryOverKModelAttachments(const char* existingPath, KModelData& model);
const KModelSocket* FindKModelSocket(const KModelData& model, const char* name);
int32_t FindKModelMaterialSlot(const KModelData& model, const char* name); // -1 if missing
void ConvertMeshDataToKModelMesh(const MeshData& mesh, KModelMesh& outMesh, const char* name = nullptr);

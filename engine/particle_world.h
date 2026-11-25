#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "engine_types.h"
#include "material.h"
#include "mesh.h"
#include "particle_sim.h"

class AssetSystem;
class CollisionWorld;
class Scene;

using ParticleSystemHandle = uint32_t;

// Every live particle system of the world. Systems are ticked once per
// frame after the entities, then turned into Scene render data
// (Scene::GetParticles). Owned by World; reach it with World::GetParticles().
class ParticleWorld
{
public:
    void Init(AssetSystem* assets, CollisionWorld* collision, Scene* scene);
    // Destroys every system and forgets the cached assets (map change).
    void Clear();

    // Loads (cached) a .kfx; null when it fails to load.
    const ParticleSystemData* LoadSystem(const std::string& path);

    // Creates and activates a system. autoDestroy: removed once it completes
    // (one-shot effects). Rotation in radians. 0 when the file fails to load.
    ParticleSystemHandle Spawn(const std::string& path, const Vector3& position, const Vector3& rotation = {}, bool autoDestroy = true,
        const Vector3& scale = { 1.0f, 1.0f, 1.0f });
    ParticleSystemInstance* Find(ParticleSystemHandle handle);
    void Destroy(ParticleSystemHandle handle);

    void Update(float dt);

    size_t GetSystemCount() const { return systems.size(); }
    uint32_t GetParticleCount() const;

private:
    struct RendererAssets
    {
        MaterialHandle material = 0;
        MeshHandle mesh = 0;
    };
    struct CachedSystem
    {
        ParticleSystemData data;
        bool loaded = false;
        // [emitter][renderer]
        std::vector<std::vector<RendererAssets>> assets;
    };
    struct Entry
    {
        std::unique_ptr<ParticleSystemInstance> instance;
        CachedSystem* cached = nullptr;
        bool autoDestroy = false;
    };

    void BuildRenderData();
    CachedSystem* LoadCached(const std::string& path);

    AssetSystem* assets = nullptr;
    CollisionWorld* collision = nullptr;
    Scene* scene = nullptr;
    std::unordered_map<std::string, std::unique_ptr<CachedSystem>> cache;
    std::unordered_map<ParticleSystemHandle, Entry> systems;
    ParticleSystemHandle nextHandle = 1;
};

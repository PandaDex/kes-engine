#include "particle_world.h"
#include "asset_system.h"
#include "collision_world.h"
#include "file_paths.h"
#include "logger.h"
#include "profiler.h"
#include "scene.h"

namespace
{
    class WorldCollision final : public IParticleCollision
    {
    public:
        explicit WorldCollision(const CollisionWorld& world) : world(world) {}

        bool Raycast(const Vector3& from, const Vector3& to, Vector3& outPoint, Vector3& outNormal) const override
        {
            const Vector3 delta = to - from;
            const float length = Vector3Length(delta);
            if (length <= 1e-5f)
                return false;
            Ray ray;
            ray.origin = from;
            ray.direction = delta / length;
            RaycastQuery query;
            query.maxDistance = length;
            RaycastHit hit;
            if (!world.Raycast(ray, hit, query) || !hit.hit)
                return false;
            outPoint = hit.point;
            outNormal = hit.normal;
            return true;
        }

    private:
        const CollisionWorld& world;
    };
}

void ParticleWorld::Init(AssetSystem* assetSystem, CollisionWorld* collisionWorld, Scene* targetScene)
{
    assets = assetSystem;
    collision = collisionWorld;
    scene = targetScene;
}

void ParticleWorld::Clear()
{
    systems.clear();
    cache.clear();
    if (scene)
        scene->GetParticles().Clear();
}

ParticleWorld::CachedSystem* ParticleWorld::LoadCached(const std::string& path)
{
    auto it = cache.find(path);
    if (it != cache.end())
        return it->second->loaded ? it->second.get() : nullptr;

    auto cached = std::make_unique<CachedSystem>();
    std::string error;
    const std::string resolved = ResolveAssetPath(path.c_str());
    cached->loaded = LoadParticleSystemFile(resolved.c_str(), cached->data, &error);
    if (!cached->loaded)
        EngineLogf("Failed to load particle system %s: %s", path.c_str(), error.c_str());
    else if (assets)
    {
        cached->assets.resize(cached->data.emitters.size());
        for (size_t e = 0; e < cached->data.emitters.size(); ++e)
        {
            for (const ParticleRendererData& renderer : cached->data.emitters[e].renderers)
            {
                RendererAssets ra;
                const ParticleParam* material = renderer.Find("Material");
                const ParticleParam* model = renderer.Find("Model");
                if (model && !model->text.empty())
                {
                    ra.mesh = assets->LoadMesh(model->text.c_str());
                    if (!material || material->text.empty())
                    {
                        const ModelHandle modelHandle = assets->LoadModel(model->text.c_str());
                        if (const KModelData* data = assets->GetModel(modelHandle); data && !data->defaultMaterial.empty())
                            ra.material = assets->LoadMaterial(data->defaultMaterial.c_str());
                    }
                }
                if (material && !material->text.empty())
                    ra.material = assets->LoadMaterial(material->text.c_str());
                cached->assets[e].push_back(ra);
            }
        }
    }
    CachedSystem* result = cached->loaded ? cached.get() : nullptr;
    cache.emplace(path, std::move(cached));
    return result;
}

const ParticleSystemData* ParticleWorld::LoadSystem(const std::string& path)
{
    CachedSystem* cached = LoadCached(path);
    return cached ? &cached->data : nullptr;
}

ParticleSystemHandle ParticleWorld::Spawn(const std::string& path, const Vector3& position, const Vector3& rotation, bool autoDestroy,
    const Vector3& scale)
{
    CachedSystem* cached = LoadCached(path);
    if (!cached)
        return 0;
    Entry entry;
    entry.instance = std::make_unique<ParticleSystemInstance>();
    entry.cached = cached;
    entry.autoDestroy = autoDestroy;
    std::vector<std::string> errors;
    entry.instance->Init(&cached->data, 0, &errors);
    for (const std::string& error : errors)
        EngineLogf("Particle system %s: %s", path.c_str(), error.c_str());
    entry.instance->SetTransform(position, rotation, scale, true);
    entry.instance->Activate(true);
    const ParticleSystemHandle handle = nextHandle++;
    systems.emplace(handle, std::move(entry));
    return handle;
}

ParticleSystemInstance* ParticleWorld::Find(ParticleSystemHandle handle)
{
    auto it = systems.find(handle);
    return it == systems.end() ? nullptr : it->second.instance.get();
}

void ParticleWorld::Destroy(ParticleSystemHandle handle)
{
    systems.erase(handle);
}

uint32_t ParticleWorld::GetParticleCount() const
{
    uint32_t total = 0;
    for (const auto& entry : systems)
        total += entry.second.instance->GetParticleCount();
    return total;
}

void ParticleWorld::Update(float dt)
{
    KES_ZONE_FN(WORLD);
    const WorldCollision worldCollision(*collision);
    for (auto it = systems.begin(); it != systems.end();)
    {
        ParticleSystemInstance& instance = *it->second.instance;
        instance.Tick(dt, collision ? &worldCollision : nullptr);
        if (it->second.autoDestroy && instance.IsComplete())
            it = systems.erase(it);
        else
            ++it;
    }
    BuildRenderData();
    KES_PROFILE_PLOT("Particles", static_cast<int64_t>(GetParticleCount()));
}

void ParticleWorld::BuildRenderData()
{
    if (!scene)
        return;
    SceneParticles& out = scene->GetParticles();
    out.Clear();
    for (const auto& pair : systems)
    {
        const Entry& entry = pair.second;
        const ParticleSystemInstance& instance = *entry.instance;
        for (size_t e = 0; e < instance.GetEmitterCount(); ++e)
        {
            const ParticleEmitterData& emitter = instance.GetEmitterData(e);
            const ParticleEmitterState& state = instance.GetEmitter(e);
            if (!emitter.enabled || state.count == 0)
                continue;
            const Vector3 center = (state.boundsMin + state.boundsMax) * 0.5f;
            for (size_t r = 0; r < emitter.renderers.size(); ++r)
            {
                const ParticleRendererData& renderer = emitter.renderers[r];
                if (!renderer.enabled)
                    continue;
                const RendererAssets ra = e < entry.cached->assets.size() && r < entry.cached->assets[e].size()
                    ? entry.cached->assets[e][r]
                    : RendererAssets{};
                if (renderer.type == "Sprite")
                {
                    SceneParticleSprites batch;
                    batch.settings = ReadParticleSpriteSettings(renderer);
                    batch.material = ra.material;
                    batch.center = center;
                    instance.ExtractSprites(e, renderer, batch.sprites);
                    if (!batch.sprites.empty())
                        out.sprites.push_back(std::move(batch));
                }
                else if (renderer.type == "Ribbon")
                {
                    SceneParticleRibbon batch;
                    batch.settings = ReadParticleRibbonSettings(renderer);
                    batch.material = ra.material;
                    batch.center = center;
                    instance.ExtractRibbon(e, renderer, batch.points);
                    if (batch.points.size() >= 2)
                        out.ribbons.push_back(std::move(batch));
                }
                else if (renderer.type == "Mesh")
                {
                    if (ra.mesh == 0)
                        continue;
                    thread_local std::vector<ParticleMeshInstance> meshes;
                    meshes.clear();
                    instance.ExtractMeshes(e, renderer, meshes);
                    for (const ParticleMeshInstance& m : meshes)
                    {
                        SceneObject object;
                        object.mesh = ra.mesh;
                        object.material = ra.material;
                        object.useWorldMatrix = true;
                        std::copy(m.world, m.world + 16, object.worldMatrix);
                        object.transform.position = { m.world[12], m.world[13], m.world[14] };
                        out.meshObjects.push_back(std::move(object));
                    }
                }
                else if (renderer.type == "Light")
                    instance.ExtractLights(e, renderer, out.lights);
            }
        }
    }
    const std::vector<ScenePointLight>& submitted = scene->GetSubmittedPointLights();
    out.lights.insert(out.lights.end(), submitted.begin(), submitted.end());
}

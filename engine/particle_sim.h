#pragma once
// CPU particle simulation of a ParticleSystemData (particle_asset.h) and the
// conversion of live particles into render data: sprite quads, ribbon strips,
// mesh instances and point lights. Shared by the engine and the editor preview.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "engine_types.h"
#include "particle_asset.h"
#include "particle_graph.h"

// Optional world collision for the Collision module (World mode).
class IParticleCollision
{
public:
    virtual ~IParticleCollision() = default;
    virtual bool Raycast(const Vector3& from, const Vector3& to, Vector3& outPoint, Vector3& outNormal) const = 0;
};

struct ParticleEmitterState
{
    struct ModuleState
    {
        uint32_t salt = 0;
        int burstsFired = 0;
        int stream = -1; // Set Attribute target
        int streamDim = 0;
    };

    uint32_t count = 0;
    std::vector<std::vector<float>> streams;
    std::vector<float*> streamPointers;
    std::vector<ModuleState> modules; // parallel to ParticleEmitterData::modules
    std::vector<std::pair<int, ParticleGraphProgram>> graphs;
    std::vector<uint8_t> kill;

    float time = 0.0f; // since activation
    float loopTime = 0.0f; // seconds into the current loop
    int loopIndex = 0;
    bool finishedSpawning = false;
    float spawnRemainder = 0.0f;
    float distanceRemainder = 0.0f;
    uint32_t nextId = 0;
    uint32_t seed = 0;
    Vector3 boundsMin;
    Vector3 boundsMax;

    const ParticleGraphProgram* FindGraph(int id) const;
};

enum class ParticleBlend : uint8_t
{
    Additive = 0,
    Alpha = 1,
    Premultiplied = 2,
};

struct ParticleSpriteSettings
{
    std::string material;
    ParticleBlend blend = ParticleBlend::Additive;
    int alignment = 0; // 0 face camera, 1 velocity, 2 axis
    int columns = 1;
    int rows = 1;
    bool sortBackToFront = false;
    bool lit = false;
    int sortOrder = 0;
};
ParticleSpriteSettings ReadParticleSpriteSettings(const ParticleRendererData& renderer);

struct ParticleRibbonSettings
{
    std::string material;
    ParticleBlend blend = ParticleBlend::Additive;
    bool tile = false;
    float tileLength = 1.0f;
    bool lit = false;
    int sortOrder = 0;
};

// Draw order of particle batches: SortOrder, then alpha-blended before additive
// (additive is order independent, so it never pops behind smoke as the camera
// moves), then far to near.
bool ParticleBatchDrawsBefore(int sortOrderA, ParticleBlend blendA, float distanceSqA, int sortOrderB, ParticleBlend blendB, float distanceSqB);
ParticleRibbonSettings ReadParticleRibbonSettings(const ParticleRendererData& renderer);

struct ParticleSpriteInstance
{
    float position[3];
    float velocity[3];
    float axis[3];
    float size[2];
    float rotation;
    float color[4]; // emissive applied, linear HDR
    float subImage;
    float stretch;
};

struct ParticleRibbonPoint
{
    float position[3];
    float width;
    float color[4];
};

struct ParticleLightInstance
{
    float position[3];
    float radius;
    float color[3]; // radiance before the renderer's light scale
};

struct ParticleMeshInstance
{
    float world[16]; // row-major, row vectors (render::Matrix4 layout)
    float color[4];
};

struct ParticleVertex
{
    float position[3];
    float uv[2];
    float color[4];
    float normal[3];
};
static_assert(sizeof(ParticleVertex) == 48, "ParticleVertex must match particle.slang");

struct ParticleCameraBasis
{
    Vector3 position;
    Vector3 right;
    Vector3 up;
    Vector3 forward;
};

// Appends quads (4 vertices, 6 indices each). Sorts `sprites` when the settings ask for it.
void BuildParticleSpriteGeometry(const ParticleSpriteSettings& settings, std::vector<ParticleSpriteInstance>& sprites,
    const ParticleCameraBasis& camera, std::vector<ParticleVertex>& vertices, std::vector<uint32_t>& indices);
// One strip through the points in order.
void BuildParticleRibbonGeometry(const ParticleRibbonSettings& settings, const std::vector<ParticleRibbonPoint>& points,
    const ParticleCameraBasis& camera, std::vector<ParticleVertex>& vertices, std::vector<uint32_t>& indices);

class ParticleSystemInstance
{
public:
    // `data` must outlive the instance (or the next Init). Graph errors are reported through outErrors
    // and the parameter then evaluates to 0.
    void Init(const ParticleSystemData* data, uint32_t seed = 0, std::vector<std::string>* outErrors = nullptr);
    const ParticleSystemData* GetData() const { return data; }

    // Rotation in radians (entity convention). teleport: no trail or inherited velocity from the jump.
    void SetTransform(const Vector3& position, const Vector3& rotation, const Vector3& scale = { 1.0f, 1.0f, 1.0f }, bool teleport = false);
    const Vector3& GetPosition() const { return position; }

    void Activate(bool reset = true);
    void Deactivate(); // stops spawning; live particles finish their lives
    void Clear(); // removes every particle
    bool IsActive() const { return active; }
    // Inactive (or every emitter done spawning) and no particle left.
    bool IsComplete() const;

    bool SetUserFloat(const std::string& name, float value);
    float GetUserFloat(const std::string& name) const;
    void SetTimeScale(float value) { timeScale = value; }

    void Tick(float dt, const IParticleCollision* collision = nullptr);

    size_t GetEmitterCount() const { return emitters.size(); }
    const ParticleEmitterState& GetEmitter(size_t index) const { return emitters[index]; }
    const ParticleEmitterData& GetEmitterData(size_t index) const { return data->emitters[index]; }
    uint32_t GetParticleCount() const;
    // World-space box around every live particle; false when there are none.
    bool GetBounds(Vector3& outMin, Vector3& outMax) const;

    void ExtractSprites(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleSpriteInstance>& out) const;
    void ExtractRibbon(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleRibbonPoint>& out) const;
    void ExtractMeshes(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleMeshInstance>& out) const;
    void ExtractLights(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleLightInstance>& out) const;

private:
    struct Rotation3
    {
        float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // row-major, row vector convention
    };

    void ResetEmitter(size_t index);
    void TickEmitter(size_t index, float dt, const IParticleCollision* collision);
    void UpdateParticles(size_t index, float dt, const IParticleCollision* collision);
    uint32_t EvaluateSpawnCount(size_t index, float dt, uint32_t& outBurst);
    void SpawnParticles(size_t index, uint32_t rateCount, uint32_t burstCount, float dt);
    void RunModule(size_t emitterIndex, size_t moduleIndex, uint32_t begin, uint32_t count, float dt, const IParticleCollision* collision);
    void Grow(ParticleEmitterState& e, uint32_t capacity);
    void Compact(ParticleEmitterState& e);
    void UpdateBounds(ParticleEmitterState& e, const ParticleEmitterData& d);

    // Writes count float4s. perParticle false: one emitter-level value (count must be 1).
    void Evaluate(size_t emitter, const ParticleValue& value, uint32_t salt, uint32_t begin, uint32_t count, bool perParticle, float* out) const;
    const float* EvaluateParam(size_t emitter, const ParticleModuleData& module, uint32_t salt, const char* name, uint32_t begin,
        uint32_t count, bool perParticle, int slot) const;
    ParticleEvalContext MakeContext(size_t emitter, bool perParticle) const;

    Vector3 ToWorldPoint(const ParticleEmitterData& d, const Vector3& p) const;
    Vector3 ToWorldVector(const ParticleEmitterData& d, const Vector3& v) const;
    Vector3 ToSimVector(const ParticleEmitterData& d, const Vector3& worldVector) const;
    static Vector3 Rotate(const Rotation3& r, const Vector3& v);
    static Vector3 RotateInverse(const Rotation3& r, const Vector3& v);
    static Rotation3 MakeRotation(const Vector3& radians);

    const ParticleSystemData* data = nullptr;
    std::vector<ParticleEmitterState> emitters;
    std::vector<float> userValues;
    Vector3 position;
    Vector3 previousPosition;
    Vector3 rotation;
    Vector3 scale = { 1.0f, 1.0f, 1.0f };
    Rotation3 rotationMatrix;
    Vector3 velocity; // of the system itself, for Inherit Velocity
    bool active = false;
    bool transformSet = false;
    float timeScale = 1.0f;
    float lastDelta = 0.0f;
    uint32_t baseSeed = 0;
    uint32_t frameCounter = 0;
    mutable std::vector<std::vector<float>> scratch;
};

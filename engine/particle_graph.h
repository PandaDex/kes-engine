#pragma once
// Particle node graphs ("dynamic inputs"): a ParticleGraphData is compiled into
// a ParticleGraphProgram and evaluated for a batch of particles at once. Every
// value is a float4 per particle; scalars are splatted to all four components.

#include <cstdint>
#include <string>
#include <vector>
#include "particle_asset.h"

// Attribute streams of an emitter (one float array each). Custom attributes
// follow kBuiltinStreams, four streams each.
namespace ParticleAttr
{
    constexpr int Position = 0; // 3
    constexpr int Velocity = 3; // 3
    constexpr int Color = 6; // 4
    constexpr int SpriteSize = 10; // 2
    constexpr int SpriteRotation = 12; // radians
    constexpr int RotationRate = 13; // radians per second
    constexpr int Age = 14;
    constexpr int Lifetime = 15;
    constexpr int Mass = 16;
    constexpr int MeshRotation = 17; // 3, radians
    constexpr int MeshAngularVelocity = 20; // 3
    constexpr int MeshScale = 23; // 3
    constexpr int SubImage = 26;
    constexpr int NormalizedAge = 27;
    constexpr int Random = 28; // 0-1, fixed per particle
    constexpr int Id = 29; // spawn index
    constexpr int InitialColor = 30; // 4
    constexpr int InitialSize = 34; // 2
    constexpr int InitialMeshScale = 36; // 3
    constexpr int kBuiltinStreams = 39;
}

struct ParticleAttributeInfo
{
    const char* name;
    int stream;
    int dim;
};
// Attributes Set Attribute and graphs may address by name.
const std::vector<ParticleAttributeInfo>& ParticleNamedAttributes();
// Built in or custom (custom streams start at kBuiltinStreams); false when unknown.
bool FindParticleAttribute(const ParticleEmitterData& emitter, const std::string& name, int& outStream, int& outDim);

uint32_t ParticleHash(uint32_t x);
inline float ParticleHash01(uint32_t x)
{
    return static_cast<float>(ParticleHash(x) >> 8) * (1.0f / 16777216.0f);
}
float ParticleValueNoise(float x, float y, float z); // -1..1, smooth
void ParticleVectorNoise(float x, float y, float z, float out[3]);

// What a graph or value can read besides the particle streams.
struct ParticleEvalContext
{
    float* const* streams = nullptr; // null for emitter-level evaluation (EmitterUpdate stage)
    float emitterTime = 0.0f; // seconds since activation
    float loopTime = 0.0f; // 0-1 inside the current loop (0 for endless emitters)
    float deltaTime = 0.0f;
    float emitterPosition[3] = {};
    const ParticleSystemData* system = nullptr;
    const float* userValues = nullptr; // parallel to system->userParams
    uint32_t frameSeed = 0; // changes every frame
};

struct ParticleGraphPortDef
{
    std::string name;
    int dim = 1; // 0 = generic (as wide as the widest input)
    float def[4] = {};
};

struct ParticleGraphParamDef
{
    std::string name;
    ParticleParamKind kind = ParticleParamKind::Text;
    std::string defaultText;
    std::string options; // Enum: "A|B"
    int dim = 1; // ParticleParamKind::Value: 1 = float, 4 = color
};

struct ParticleGraphNodeDef
{
    std::string type;
    std::string title;
    std::string category;
    std::string description;
    std::vector<ParticleGraphPortDef> inputs;
    std::vector<ParticleGraphPortDef> outputs;
    std::vector<ParticleGraphParamDef> params;
    bool isOutput = false;
};

const std::vector<ParticleGraphNodeDef>& ParticleGraphCatalog();
const ParticleGraphNodeDef* FindParticleGraphNodeDef(const std::string& type);

class ParticleGraphProgram
{
public:
    // False (with a reason) when the graph has no connected output or an unknown node.
    bool Compile(const ParticleGraphData& graph, const ParticleEmitterData& emitter, const ParticleSystemData& system, std::string* outError);
    bool IsValid() const { return valid; }
    int OutputDim() const { return outputDim; }
    // Writes count float4 values to out. Particles begin..begin+count when ctx.streams is set.
    void Run(const ParticleEvalContext& ctx, uint32_t begin, uint32_t count, float* out) const;

    struct Source
    {
        int slot = -1; // -1: the constant
        int dim = 1;
        float value[4] = {};
    };
    struct Op
    {
        int code = 0;
        int dim = 1; // result width
        int out = 0; // slot (first of `outputs` for multi-output ops)
        int outputs = 1;
        Source in[5];
        int inputCount = 0;
        int stream = -1; // attribute reads
        int user = -1; // user parameter index
        uint32_t seed = 0;
        int mode = 0;
        std::vector<ParticleCurveKey> curve;
        std::vector<ParticleGradientKey> gradient;
        float constant[4] = {};
    };

private:
    std::vector<Op> ops;
    Source result;
    int slotCount = 0;
    int outputDim = 1;
    bool valid = false;
};

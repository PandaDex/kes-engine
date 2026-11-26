#pragma once
// Particle system assets (.kfx): a system holds emitters; an emitter holds a
// stack of modules per stage (like Niagara) and a list of renderers. Module and
// renderer parameters are ParticleValues: a constant, a random range, a curve,
// a gradient or a small node graph (particle_graph.h) owned by the emitter.
//
// Only depends on text_block_file and engine_types so the KES Editor can
// compile it and preview exactly what the game simulates.

#include <cstdint>
#include <string>
#include <vector>

enum class ParticleStage : uint8_t
{
    EmitterUpdate = 0, // once per emitter per frame: spawn rates and bursts
    ParticleSpawn = 1, // once for every new particle
    ParticleUpdate = 2, // every particle, every frame
};
constexpr int kParticleStageCount = 3;
const char* ParticleStageName(ParticleStage stage);
bool ParseParticleStage(const std::string& text, ParticleStage& out);

enum class ParticleValueMode : uint8_t
{
    Constant = 0,
    Random = 1, // a + (b - a) * random, stable per particle
    Curve = 2, // a * curve(input)
    Gradient = 3, // gradient(input), rgba
    Graph = 4, // node graph output
};

enum class ParticleCurveInput : uint8_t
{
    NormalizedAge = 0,
    Age = 1,
    Speed = 2,
    Random = 3,
    EmitterTime = 4,
    NormalizedLoopTime = 5,
};
const char* ParticleCurveInputName(ParticleCurveInput input);

struct ParticleCurveKey
{
    float time = 0.0f;
    float value = 0.0f;
};

struct ParticleGradientKey
{
    float time = 0.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
};

struct ParticleValue
{
    ParticleValueMode mode = ParticleValueMode::Constant;
    float a[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float b[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool uniform = true; // Random: one random number for every component (keeps ratios)
    ParticleCurveInput input = ParticleCurveInput::NormalizedAge;
    std::vector<ParticleCurveKey> curve;
    std::vector<ParticleGradientKey> gradient;
    int graph = 0; // ParticleGraphData::id in the emitter

    static ParticleValue Constant(float x, float y = 0.0f, float z = 0.0f, float w = 0.0f);
};

float EvaluateParticleCurve(const std::vector<ParticleCurveKey>& keys, float x);
void EvaluateParticleGradient(const std::vector<ParticleGradientKey>& keys, float x, float out[4]);

// "const 1 2 3", "random uniform 0 0 ; 1 1", "curve NormalizedAge 1 ; 0 0 ; 1 1",
// "gradient NormalizedAge ; 0 1 1 1 1 ; 1 1 1 1 0", "graph 3". A bare number list is a constant.
std::string FormatParticleValue(const ParticleValue& value, int dim);
bool ParseParticleValue(const std::string& text, int dim, ParticleValue& out);

enum class ParticleParamKind : uint8_t
{
    Value, // ParticleValue with `dim` components
    Enum, // one of `options` ("A|B|C")
    Bool,
    Int,
    Asset, // content path (material, model)
    Text,
};

struct ParticleParamDef
{
    std::string name;
    ParticleParamKind kind = ParticleParamKind::Value;
    int dim = 1;
    std::string defaultText; // value text (FormatParticleValue) or the option/bool/int/text
    std::string options; // Enum: "A|B|C"; Asset: file extensions "mat|kmodel"
    std::string tooltip;
    bool degrees = false; // shown and stored in degrees, converted by the module
};

struct ParticleModuleDef
{
    std::string type;
    std::string title;
    std::string category;
    std::string description;
    uint32_t stageMask = 0; // bit per ParticleStage; 0 = renderer
    std::vector<ParticleParamDef> params;

    const ParticleParamDef* FindParam(const std::string& name) const;
    bool AllowsStage(ParticleStage stage) const { return (stageMask & (1u << static_cast<uint32_t>(stage))) != 0; }
};

const std::vector<ParticleModuleDef>& ParticleModuleCatalog();
const std::vector<ParticleModuleDef>& ParticleRendererCatalog();
const ParticleModuleDef* FindParticleModuleDef(const std::string& type);
const ParticleModuleDef* FindParticleRendererDef(const std::string& type);

struct ParticleParam
{
    std::string name;
    ParticleValue value; // ParticleParamKind::Value
    std::string text; // every other kind
};

struct ParticleModuleData
{
    std::string type;
    ParticleStage stage = ParticleStage::ParticleUpdate;
    bool enabled = true;
    std::vector<ParticleParam> params;

    const ParticleParam* Find(const std::string& name) const;
    ParticleParam* Find(const std::string& name);
};

// A renderer reuses the module layout; its stage is ignored.
using ParticleRendererData = ParticleModuleData;

// Missing parameters get their defaults, unknown ones are kept as they are.
void ApplyParticleDefaults(ParticleModuleData& module, const ParticleModuleDef& def);
ParticleModuleData MakeParticleModule(const std::string& type, ParticleStage stage);
ParticleRendererData MakeParticleRenderer(const std::string& type);

struct ParticleGraphNode
{
    int id = 0;
    std::string type;
    float x = 0.0f;
    float y = 0.0f;
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<std::pair<int, std::vector<float>>> inputDefaults; // input port -> value

    const std::string* FindParam(const std::string& name) const;
    const std::vector<float>* FindInputDefault(int port) const;
};

struct ParticleGraphLink
{
    int fromNode = 0;
    int fromPort = 0;
    int toNode = 0;
    int toPort = 0;
};

struct ParticleGraphData
{
    int id = 0;
    std::vector<ParticleGraphNode> nodes;
    std::vector<ParticleGraphLink> links;
};

struct ParticleEmitterData
{
    std::string name = "Emitter";
    bool enabled = true;
    bool localSpace = false; // particles follow the system's transform
    uint32_t maxParticles = 1000;
    float duration = 0.0f; // seconds per loop, 0 = endless
    bool loop = true;
    uint32_t loopCount = 0; // 0 = forever
    float delay = 0.0f;
    uint32_t seed = 0; // 0 = different every activation
    std::vector<std::string> customAttributes; // float4 each, readable and writable by graphs and Set Attribute
    std::vector<ParticleModuleData> modules; // grouped by stage when saved; order inside a stage is the stack order
    std::vector<ParticleRendererData> renderers;
    std::vector<ParticleGraphData> graphs;

    const ParticleGraphData* FindGraph(int id) const;
    int NextGraphId() const;
};

struct ParticleUserParam
{
    std::string name;
    float value = 0.0f;
};

struct ParticleSystemData
{
    std::vector<ParticleEmitterData> emitters;
    std::vector<ParticleUserParam> userParams; // set from game code / MapScript, read by graphs
    float warmup = 0.0f; // seconds simulated on activation
};

bool LoadParticleSystemFile(const char* path, ParticleSystemData& out, std::string* outError = nullptr);
std::string WriteParticleSystemText(const ParticleSystemData& system);

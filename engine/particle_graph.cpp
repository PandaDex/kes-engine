#include "particle_graph.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <unordered_map>

namespace
{
    enum OpCode
    {
        OpAttr,
        OpSpeed,
        OpEmitterTime,
        OpLoopTime,
        OpDeltaTime,
        OpEmitterPosition,
        OpUser,
        OpConstant,
        OpAdd,
        OpSubtract,
        OpMultiply,
        OpDivide,
        OpMin,
        OpMax,
        OpPower,
        OpLerp,
        OpClamp,
        OpSaturate,
        OpAbs,
        OpNegate,
        OpOneMinus,
        OpSin,
        OpCos,
        OpFrac,
        OpFloor,
        OpSqrt,
        OpStep,
        OpSmoothstep,
        OpRemap,
        OpLength,
        OpNormalize,
        OpDot,
        OpCross,
        OpDistance,
        OpMake3,
        OpMake4,
        OpSplit,
        OpRandom,
        OpNoise,
        OpVectorNoise,
        OpCurve,
        OpGradient,
        OpSine,
    };

    constexpr int kLanes = 64;

    ParticleGraphPortDef P(const char* name, int dim, float d0 = 0.0f, float d1 = 0.0f, float d2 = 0.0f, float d3 = 0.0f)
    {
        ParticleGraphPortDef p;
        p.name = name;
        p.dim = dim;
        p.def[0] = d0;
        p.def[1] = d1;
        p.def[2] = d2;
        p.def[3] = d3;
        return p;
    }

    ParticleGraphParamDef Param(const char* name, ParticleParamKind kind, const char* def, const char* options = "", int dim = 1)
    {
        ParticleGraphParamDef p;
        p.name = name;
        p.kind = kind;
        p.defaultText = def;
        p.options = options;
        p.dim = dim;
        return p;
    }

    struct NodeBuilder
    {
        std::vector<ParticleGraphNodeDef>& list;
        ParticleGraphNodeDef& add(const char* type, const char* title, const char* category, const char* description)
        {
            ParticleGraphNodeDef d;
            d.type = type;
            d.title = title;
            d.category = category;
            d.description = description;
            list.push_back(d);
            return list.back();
        }
    };

    std::vector<ParticleGraphNodeDef> BuildCatalog()
    {
        std::vector<ParticleGraphNodeDef> c;
        NodeBuilder b{ c };
        {
            ParticleGraphNodeDef& d = b.add("Output", "Output", "Output", "The value the parameter gets.");
            d.inputs = { P("Value", 0) };
            d.isOutput = true;
        }
        {
            ParticleGraphNodeDef& d = b.add("ParticleAttributes", "Particle Attributes", "Inputs", "Values of the particle being evaluated.");
            d.outputs = { P("Position", 3), P("Velocity", 3), P("Color", 4), P("SpriteSize", 2), P("Age", 1), P("NormalizedAge", 1),
                P("Lifetime", 1), P("Random", 1), P("Mass", 1), P("Id", 1), P("Speed", 1) };
        }
        {
            ParticleGraphNodeDef& d = b.add("CustomAttribute", "Custom Attribute", "Inputs", "An attribute listed in the emitter's Custom Attributes.");
            d.outputs = { P("Value", 4) };
            d.params = { Param("Name", ParticleParamKind::Text, "") };
        }
        {
            ParticleGraphNodeDef& d = b.add("Emitter", "Emitter", "Inputs", "Emitter time and position.");
            d.outputs = { P("Time", 1), P("LoopTime", 1), P("DeltaTime", 1), P("Position", 3) };
        }
        {
            ParticleGraphNodeDef& d = b.add("UserParameter", "User Parameter", "Inputs", "A system user parameter (set by game code or MapScript).");
            d.outputs = { P("Value", 1) };
            d.params = { Param("Name", ParticleParamKind::Text, "") };
        }
        {
            ParticleGraphNodeDef& d = b.add("Float", "Float", "Constants", "A number.");
            d.outputs = { P("Value", 1) };
            d.params = { Param("Value", ParticleParamKind::Value, "1") };
        }
        {
            ParticleGraphNodeDef& d = b.add("Vector", "Vector", "Constants", "A 3D vector.");
            d.outputs = { P("Value", 3) };
            d.params = { Param("X", ParticleParamKind::Value, "0"), Param("Y", ParticleParamKind::Value, "1"), Param("Z", ParticleParamKind::Value, "0") };
        }
        {
            ParticleGraphNodeDef& d = b.add("Color", "Color", "Constants", "An RGBA color (HDR allowed).");
            d.outputs = { P("Value", 4) };
            d.params = { Param("Color", ParticleParamKind::Value, "1 1 1 1", "", 4) };
        }

        auto binary = [&](const char* type, const char* title, const char* desc, float defB)
        {
            ParticleGraphNodeDef& d = b.add(type, title, "Math", desc);
            d.inputs = { P("A", 0), P("B", 0, defB, defB, defB, defB) };
            d.outputs = { P("Result", 0) };
        };
        binary("Add", "Add", "A + B", 0.0f);
        binary("Subtract", "Subtract", "A - B", 0.0f);
        binary("Multiply", "Multiply", "A * B", 1.0f);
        binary("Divide", "Divide", "A / B", 1.0f);
        binary("Min", "Min", "Smaller of A and B", 0.0f);
        binary("Max", "Max", "Larger of A and B", 0.0f);
        {
            ParticleGraphNodeDef& d = b.add("Power", "Power", "Math", "Base ^ Exponent");
            d.inputs = { P("Base", 0), P("Exponent", 0, 2, 2, 2, 2) };
            d.outputs = { P("Result", 0) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Lerp", "Lerp", "Math", "A + (B - A) * T");
            d.inputs = { P("A", 0), P("B", 0, 1, 1, 1, 1), P("T", 0, 0.5f, 0.5f, 0.5f, 0.5f) };
            d.outputs = { P("Result", 0) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Clamp", "Clamp", "Math", "X limited to Min..Max");
            d.inputs = { P("X", 0), P("Min", 0), P("Max", 0, 1, 1, 1, 1) };
            d.outputs = { P("Result", 0) };
        }
        auto unary = [&](const char* type, const char* title, const char* desc)
        {
            ParticleGraphNodeDef& d = b.add(type, title, "Math", desc);
            d.inputs = { P("X", 0) };
            d.outputs = { P("Result", 0) };
        };
        unary("Saturate", "Saturate", "Clamp to 0..1");
        unary("Abs", "Abs", "Absolute value");
        unary("Negate", "Negate", "-X");
        unary("OneMinus", "One Minus", "1 - X");
        unary("Sin", "Sin", "Sine (radians)");
        unary("Cos", "Cos", "Cosine (radians)");
        unary("Frac", "Frac", "Fractional part");
        unary("Floor", "Floor", "Round down");
        unary("Sqrt", "Sqrt", "Square root");
        {
            ParticleGraphNodeDef& d = b.add("Step", "Step", "Math", "1 where X >= Edge, else 0");
            d.inputs = { P("Edge", 0, 0.5f, 0.5f, 0.5f, 0.5f), P("X", 0) };
            d.outputs = { P("Result", 0) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Smoothstep", "Smoothstep", "Math", "Smooth 0..1 between Edge0 and Edge1");
            d.inputs = { P("Edge0", 0), P("Edge1", 0, 1, 1, 1, 1), P("X", 0) };
            d.outputs = { P("Result", 0) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Remap", "Remap", "Math", "X from InMin..InMax to OutMin..OutMax");
            d.inputs = { P("X", 0), P("InMin", 0), P("InMax", 0, 1, 1, 1, 1), P("OutMin", 0), P("OutMax", 0, 1, 1, 1, 1) };
            d.outputs = { P("Result", 0) };
        }
        {
            ParticleGraphNodeDef& d = b.add("SineWave", "Sine Wave", "Math", "Amplitude * sin(2 pi (X * Frequency + Phase)), 0..1 when Normalized");
            d.inputs = { P("X", 1), P("Frequency", 1, 1), P("Phase", 1), P("Amplitude", 1, 1) };
            d.outputs = { P("Result", 1) };
            d.params = { Param("Normalized", ParticleParamKind::Bool, "false") };
        }

        {
            ParticleGraphNodeDef& d = b.add("Length", "Length", "Vector", "Length of a vector");
            d.inputs = { P("V", 0) };
            d.outputs = { P("Result", 1) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Normalize", "Normalize", "Vector", "Unit vector");
            d.inputs = { P("V", 0) };
            d.outputs = { P("Result", 0) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Dot", "Dot", "Vector", "Dot product");
            d.inputs = { P("A", 0), P("B", 0) };
            d.outputs = { P("Result", 1) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Cross", "Cross", "Vector", "Cross product");
            d.inputs = { P("A", 3), P("B", 3, 0, 1, 0) };
            d.outputs = { P("Result", 3) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Distance", "Distance", "Vector", "Distance between two points");
            d.inputs = { P("A", 0), P("B", 0) };
            d.outputs = { P("Result", 1) };
        }
        {
            ParticleGraphNodeDef& d = b.add("MakeVector", "Make Vector", "Vector", "Vector from X, Y, Z");
            d.inputs = { P("X", 1), P("Y", 1), P("Z", 1) };
            d.outputs = { P("Value", 3) };
        }
        {
            ParticleGraphNodeDef& d = b.add("MakeColor", "Make Color", "Vector", "Color from R, G, B, A");
            d.inputs = { P("R", 1, 1), P("G", 1, 1), P("B", 1, 1), P("A", 1, 1) };
            d.outputs = { P("Value", 4) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Split", "Split", "Vector", "Components of a vector");
            d.inputs = { P("V", 0) };
            d.outputs = { P("X", 1), P("Y", 1), P("Z", 1), P("W", 1) };
        }
        {
            ParticleGraphNodeDef& d = b.add("RandomRange", "Random Range", "Random", "Random value between Min and Max");
            d.inputs = { P("Min", 0), P("Max", 0, 1, 1, 1, 1) };
            d.outputs = { P("Value", 0) };
            d.params = { Param("Mode", ParticleParamKind::Enum, "PerParticle", "PerParticle|PerFrame"), Param("Seed", ParticleParamKind::Int, "0") };
        }
        {
            ParticleGraphNodeDef& d = b.add("Noise", "Noise", "Random", "Smooth 3D noise, -1..1");
            d.inputs = { P("Position", 3), P("Frequency", 1, 1) };
            d.outputs = { P("Value", 1), P("Vector", 3) };
        }
        {
            ParticleGraphNodeDef& d = b.add("Curve", "Curve", "Curves", "Curve through keys (\"time value; time value\")");
            d.inputs = { P("X", 1) };
            d.outputs = { P("Value", 1) };
            d.params = { Param("Keys", ParticleParamKind::Text, "0 0; 1 1") };
        }
        {
            ParticleGraphNodeDef& d = b.add("Gradient", "Gradient", "Curves", "Color gradient (\"time r g b a; ...\")");
            d.inputs = { P("X", 1) };
            d.outputs = { P("Value", 4) };
            d.params = { Param("Keys", ParticleParamKind::Text, "0 1 1 1 1; 1 1 1 1 0") };
        }
        return c;
    }

    std::vector<float> ParseFloats(const std::string& text)
    {
        std::vector<float> out;
        std::string cleaned = text;
        std::replace(cleaned.begin(), cleaned.end(), ',', ' ');
        std::istringstream s(cleaned);
        float f = 0.0f;
        while (s >> f)
            out.push_back(f);
        return out;
    }

    std::vector<std::string> SplitSemicolons(const std::string& text)
    {
        std::vector<std::string> out;
        std::string current;
        for (char ch : text)
        {
            if (ch == ';')
            {
                out.push_back(current);
                current.clear();
            }
            else
                current.push_back(ch);
        }
        out.push_back(current);
        return out;
    }

    int OpCodeOf(const std::string& type)
    {
        static const std::unordered_map<std::string, int> codes = {
            { "Add", OpAdd }, { "Subtract", OpSubtract }, { "Multiply", OpMultiply }, { "Divide", OpDivide }, { "Min", OpMin },
            { "Max", OpMax }, { "Power", OpPower }, { "Lerp", OpLerp }, { "Clamp", OpClamp }, { "Saturate", OpSaturate },
            { "Abs", OpAbs }, { "Negate", OpNegate }, { "OneMinus", OpOneMinus }, { "Sin", OpSin }, { "Cos", OpCos },
            { "Frac", OpFrac }, { "Floor", OpFloor }, { "Sqrt", OpSqrt }, { "Step", OpStep }, { "Smoothstep", OpSmoothstep },
            { "Remap", OpRemap }, { "Length", OpLength }, { "Normalize", OpNormalize }, { "Dot", OpDot }, { "Cross", OpCross },
            { "Distance", OpDistance }, { "MakeVector", OpMake3 }, { "MakeColor", OpMake4 }, { "Split", OpSplit },
            { "RandomRange", OpRandom }, { "Curve", OpCurve }, { "Gradient", OpGradient }, { "SineWave", OpSine },
        };
        auto it = codes.find(type);
        return it == codes.end() ? -1 : it->second;
    }

    float Fade(float t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    float LatticeValue(int x, int y, int z)
    {
        const uint32_t h = ParticleHash(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u);
        return static_cast<float>(h >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
}

const std::vector<ParticleAttributeInfo>& ParticleNamedAttributes()
{
    static const std::vector<ParticleAttributeInfo> attributes = {
        { "Position", ParticleAttr::Position, 3 },
        { "Velocity", ParticleAttr::Velocity, 3 },
        { "Color", ParticleAttr::Color, 4 },
        { "SpriteSize", ParticleAttr::SpriteSize, 2 },
        { "SpriteRotation", ParticleAttr::SpriteRotation, 1 },
        { "SpriteRotationRate", ParticleAttr::RotationRate, 1 },
        { "Age", ParticleAttr::Age, 1 },
        { "Lifetime", ParticleAttr::Lifetime, 1 },
        { "Mass", ParticleAttr::Mass, 1 },
        { "MeshRotation", ParticleAttr::MeshRotation, 3 },
        { "MeshAngularVelocity", ParticleAttr::MeshAngularVelocity, 3 },
        { "MeshScale", ParticleAttr::MeshScale, 3 },
        { "SubImage", ParticleAttr::SubImage, 1 },
        { "NormalizedAge", ParticleAttr::NormalizedAge, 1 },
        { "Random", ParticleAttr::Random, 1 },
        { "Id", ParticleAttr::Id, 1 },
    };
    return attributes;
}

bool FindParticleAttribute(const ParticleEmitterData& emitter, const std::string& name, int& outStream, int& outDim)
{
    for (const ParticleAttributeInfo& a : ParticleNamedAttributes())
    {
        if (name == a.name)
        {
            outStream = a.stream;
            outDim = a.dim;
            return true;
        }
    }
    for (size_t i = 0; i < emitter.customAttributes.size(); ++i)
    {
        if (emitter.customAttributes[i] == name)
        {
            outStream = ParticleAttr::kBuiltinStreams + static_cast<int>(i) * 4;
            outDim = 4;
            return true;
        }
    }
    return false;
}

uint32_t ParticleHash(uint32_t x)
{
    // PCG output permutation
    uint32_t state = x * 747796405u + 2891336453u;
    uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float ParticleValueNoise(float x, float y, float z)
{
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float tx = Fade(x - fx), ty = Fade(y - fy), tz = Fade(z - fz);
    float c[2][2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                c[dz][dy][dx] = LatticeValue(ix + dx, iy + dy, iz + dz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float y0 = lerp(lerp(c[0][0][0], c[0][0][1], tx), lerp(c[0][1][0], c[0][1][1], tx), ty);
    const float y1 = lerp(lerp(c[1][0][0], c[1][0][1], tx), lerp(c[1][1][0], c[1][1][1], tx), ty);
    return lerp(y0, y1, tz);
}

void ParticleVectorNoise(float x, float y, float z, float out[3])
{
    out[0] = ParticleValueNoise(x, y, z);
    out[1] = ParticleValueNoise(x + 31.416f, y - 47.853f, z + 12.793f);
    out[2] = ParticleValueNoise(x - 19.271f, y + 27.389f, z - 61.147f);
}

const std::vector<ParticleGraphNodeDef>& ParticleGraphCatalog()
{
    static const std::vector<ParticleGraphNodeDef> catalog = BuildCatalog();
    return catalog;
}

const ParticleGraphNodeDef* FindParticleGraphNodeDef(const std::string& type)
{
    for (const ParticleGraphNodeDef& d : ParticleGraphCatalog())
    {
        if (d.type == type)
            return &d;
    }
    return nullptr;
}

bool ParticleGraphProgram::Compile(const ParticleGraphData& graph, const ParticleEmitterData& emitter, const ParticleSystemData& system,
    std::string* outError)
{
    ops.clear();
    slotCount = 0;
    valid = false;
    auto fail = [&](const std::string& why)
    {
        if (outError)
            *outError = why;
        return false;
    };

    std::unordered_map<int, const ParticleGraphNode*> nodes;
    const ParticleGraphNode* output = nullptr;
    for (const ParticleGraphNode& n : graph.nodes)
    {
        nodes[n.id] = &n;
        const ParticleGraphNodeDef* def = FindParticleGraphNodeDef(n.type);
        if (!def)
            return fail("Unknown graph node \"" + n.type + "\"");
        if (def->isOutput)
            output = &n;
    }
    if (!output)
        return fail("The graph has no Output node");

    auto inputLink = [&](int node, int port) -> const ParticleGraphLink*
    {
        for (const ParticleGraphLink& l : graph.links)
        {
            if (l.toNode == node && l.toPort == port)
                return &l;
        }
        return nullptr;
    };

    // Post-order from the output; 1 = visiting, 2 = done.
    std::vector<const ParticleGraphNode*> order;
    std::unordered_map<int, int> state;
    bool cycle = false;
    auto visit = [&](auto& self, const ParticleGraphNode* n) -> void
    {
        int& s = state[n->id];
        if (s == 2)
            return;
        if (s == 1)
        {
            cycle = true;
            return;
        }
        s = 1;
        const ParticleGraphNodeDef* def = FindParticleGraphNodeDef(n->type);
        for (int port = 0; port < static_cast<int>(def->inputs.size()); ++port)
        {
            if (const ParticleGraphLink* l = inputLink(n->id, port))
            {
                auto it = nodes.find(l->fromNode);
                if (it != nodes.end())
                    self(self, it->second);
            }
        }
        state[n->id] = 2;
        order.push_back(n);
    };
    visit(visit, output);
    if (cycle)
        return fail("The graph has a loop");

    struct OutInfo
    {
        int slot;
        int dim;
    };
    std::unordered_map<int, std::vector<OutInfo>> outs;

    for (const ParticleGraphNode* n : order)
    {
        const ParticleGraphNodeDef& def = *FindParticleGraphNodeDef(n->type);
        Source sources[5];
        int maxDim = 1;
        const int inputCount = std::min<int>(5, static_cast<int>(def.inputs.size()));
        for (int port = 0; port < inputCount; ++port)
        {
            Source& src = sources[port];
            const ParticleGraphLink* l = inputLink(n->id, port);
            auto from = l ? outs.find(l->fromNode) : outs.end();
            if (from != outs.end() && l->fromPort >= 0 && l->fromPort < static_cast<int>(from->second.size()))
            {
                src.slot = from->second[static_cast<size_t>(l->fromPort)].slot;
                src.dim = from->second[static_cast<size_t>(l->fromPort)].dim;
            }
            else
            {
                const std::vector<float>* d = n->FindInputDefault(port);
                const int declared = def.inputs[static_cast<size_t>(port)].dim;
                int count = d && !d->empty() ? std::min<int>(4, static_cast<int>(d->size())) : (declared > 0 ? declared : 1);
                for (int c = 0; c < 4; ++c)
                {
                    float v = def.inputs[static_cast<size_t>(port)].def[c];
                    if (d && !d->empty())
                        v = c < static_cast<int>(d->size()) ? (*d)[static_cast<size_t>(c)] : (count == 1 ? (*d)[0] : 0.0f);
                    src.value[c] = v;
                }
                if (count == 1)
                    src.value[1] = src.value[2] = src.value[3] = src.value[0];
                src.dim = count;
            }
            maxDim = std::max(maxDim, src.dim);
        }

        if (def.isOutput)
        {
            result = sources[0];
            outputDim = sources[0].dim;
            continue;
        }

        std::vector<OutInfo>& nodeOuts = outs[n->id];
        auto newOp = [&](int code, int dim) -> Op&
        {
            Op op;
            op.code = code;
            op.dim = dim;
            op.out = slotCount++;
            op.inputCount = inputCount;
            for (int i = 0; i < inputCount; ++i)
                op.in[i] = sources[i];
            ops.push_back(std::move(op));
            nodeOuts.push_back({ ops.back().out, dim });
            return ops.back();
        };
        auto param = [&](const char* name) -> std::string
        {
            if (const std::string* v = n->FindParam(name))
                return *v;
            for (const ParticleGraphParamDef& p : def.params)
            {
                if (p.name == name)
                    return p.defaultText;
            }
            return {};
        };

        if (n->type == "ParticleAttributes")
        {
            const int streams[] = { ParticleAttr::Position, ParticleAttr::Velocity, ParticleAttr::Color, ParticleAttr::SpriteSize,
                ParticleAttr::Age, ParticleAttr::NormalizedAge, ParticleAttr::Lifetime, ParticleAttr::Random, ParticleAttr::Mass,
                ParticleAttr::Id };
            for (size_t i = 0; i < def.outputs.size(); ++i)
            {
                if (def.outputs[i].name == "Speed")
                    newOp(OpSpeed, 1);
                else
                    newOp(OpAttr, def.outputs[i].dim).stream = streams[i];
            }
        }
        else if (n->type == "CustomAttribute")
        {
            int stream = -1, dim = 4;
            if (!FindParticleAttribute(emitter, param("Name"), stream, dim))
                return fail("Unknown attribute \"" + param("Name") + "\"");
            newOp(OpAttr, dim).stream = stream;
        }
        else if (n->type == "Emitter")
        {
            newOp(OpEmitterTime, 1);
            newOp(OpLoopTime, 1);
            newOp(OpDeltaTime, 1);
            newOp(OpEmitterPosition, 3);
        }
        else if (n->type == "UserParameter")
        {
            Op& op = newOp(OpUser, 1);
            const std::string name = param("Name");
            for (size_t i = 0; i < system.userParams.size(); ++i)
            {
                if (system.userParams[i].name == name)
                    op.user = static_cast<int>(i);
            }
        }
        else if (n->type == "Float" || n->type == "Vector" || n->type == "Color")
        {
            float v[4] = {};
            int dim = 1;
            if (n->type == "Float")
            {
                const std::vector<float> f = ParseFloats(param("Value"));
                v[0] = v[1] = v[2] = v[3] = f.empty() ? 0.0f : f[0];
            }
            else if (n->type == "Vector")
            {
                dim = 3;
                const char* names[3] = { "X", "Y", "Z" };
                for (int c = 0; c < 3; ++c)
                {
                    const std::vector<float> f = ParseFloats(param(names[c]));
                    v[c] = f.empty() ? 0.0f : f[0];
                }
            }
            else
            {
                dim = 4;
                const std::vector<float> f = ParseFloats(param("Color"));
                for (int c = 0; c < 4; ++c)
                    v[c] = c < static_cast<int>(f.size()) ? f[static_cast<size_t>(c)] : 1.0f;
            }
            Op& op = newOp(OpConstant, dim);
            std::copy(v, v + 4, op.constant);
        }
        else if (n->type == "Noise")
        {
            newOp(OpNoise, 1);
            newOp(OpVectorNoise, 3);
        }
        else if (n->type == "Split")
        {
            Op& op = newOp(OpSplit, 1);
            op.outputs = 4;
            for (int i = 1; i < 4; ++i)
                nodeOuts.push_back({ slotCount++, 1 });
        }
        else
        {
            const int code = OpCodeOf(n->type);
            if (code < 0)
                return fail("Unsupported graph node \"" + n->type + "\"");
            int dim = maxDim;
            const std::string& outName = def.outputs.empty() ? std::string() : def.outputs[0].name;
            if (!def.outputs.empty() && def.outputs[0].dim > 0)
                dim = def.outputs[0].dim;
            (void)outName;
            Op& op = newOp(code, dim);
            if (code == OpRandom)
            {
                op.mode = param("Mode") == "PerFrame" ? 1 : 0;
                op.seed = ParticleHash(static_cast<uint32_t>(std::atoi(param("Seed").c_str())) ^ static_cast<uint32_t>(n->id) * 2654435761u);
            }
            else if (code == OpSine)
                op.mode = param("Normalized") == "true" ? 1 : 0;
            else if (code == OpCurve)
            {
                for (const std::string& key : SplitSemicolons(param("Keys")))
                {
                    const std::vector<float> f = ParseFloats(key);
                    if (f.size() >= 2)
                        op.curve.push_back({ f[0], f[1] });
                }
                std::sort(op.curve.begin(), op.curve.end(), [](const ParticleCurveKey& a, const ParticleCurveKey& b) { return a.time < b.time; });
            }
            else if (code == OpGradient)
            {
                for (const std::string& key : SplitSemicolons(param("Keys")))
                {
                    const std::vector<float> f = ParseFloats(key);
                    if (f.empty())
                        continue;
                    ParticleGradientKey k;
                    k.time = f[0];
                    for (int c = 0; c < 4; ++c)
                        k.color[c] = c + 1 < static_cast<int>(f.size()) ? f[static_cast<size_t>(c) + 1] : 1.0f;
                    op.gradient.push_back(k);
                }
                std::sort(op.gradient.begin(), op.gradient.end(),
                    [](const ParticleGradientKey& a, const ParticleGradientKey& b) { return a.time < b.time; });
            }
        }
    }

    valid = true;
    return true;
}

void ParticleGraphProgram::Run(const ParticleEvalContext& ctx, uint32_t begin, uint32_t count, float* out) const
{
    if (!valid)
    {
        std::fill(out, out + static_cast<size_t>(count) * 4, 0.0f);
        return;
    }

    thread_local std::vector<float> scratch;
    scratch.resize(static_cast<size_t>(std::max(slotCount, 1)) * kLanes * 4);
    float* slots = scratch.data();
    auto slotPtr = [&](int slot, int lane) { return slots + (static_cast<size_t>(slot) * kLanes + static_cast<size_t>(lane)) * 4; };
    auto fetch = [&](const Source& s, int lane) -> const float* { return s.slot >= 0 ? slotPtr(s.slot, lane) : s.value; };
    auto splat = [](float* o, float v) { o[0] = o[1] = o[2] = o[3] = v; };
    auto len = [](const float* v, int dim)
    {
        float sum = 0.0f;
        for (int c = 0; c < std::min(dim, 4); ++c)
            sum += v[c] * v[c];
        return std::sqrt(sum);
    };

    for (uint32_t chunk = 0; chunk < count; chunk += kLanes)
    {
        const int lanes = static_cast<int>(std::min<uint32_t>(kLanes, count - chunk));
        const uint32_t first = begin + chunk;

        for (const Op& op : ops)
        {
            for (int lane = 0; lane < lanes; ++lane)
            {
                float* o = slotPtr(op.out, lane);
                const uint32_t particle = first + static_cast<uint32_t>(lane);
                const float* a = op.inputCount > 0 ? fetch(op.in[0], lane) : nullptr;
                const float* b = op.inputCount > 1 ? fetch(op.in[1], lane) : nullptr;
                const float* c = op.inputCount > 2 ? fetch(op.in[2], lane) : nullptr;
                switch (op.code)
                {
                case OpAttr:
                    if (!ctx.streams)
                    {
                        splat(o, 0.0f);
                        break;
                    }
                    for (int k = 0; k < 4; ++k)
                        o[k] = k < op.dim ? ctx.streams[op.stream + k][particle] : 0.0f;
                    if (op.dim == 1)
                        splat(o, o[0]);
                    break;
                case OpSpeed:
                    if (!ctx.streams)
                    {
                        splat(o, 0.0f);
                        break;
                    }
                    {
                        const float vx = ctx.streams[ParticleAttr::Velocity][particle];
                        const float vy = ctx.streams[ParticleAttr::Velocity + 1][particle];
                        const float vz = ctx.streams[ParticleAttr::Velocity + 2][particle];
                        splat(o, std::sqrt(vx * vx + vy * vy + vz * vz));
                    }
                    break;
                case OpEmitterTime: splat(o, ctx.emitterTime); break;
                case OpLoopTime: splat(o, ctx.loopTime); break;
                case OpDeltaTime: splat(o, ctx.deltaTime); break;
                case OpEmitterPosition:
                    o[0] = ctx.emitterPosition[0];
                    o[1] = ctx.emitterPosition[1];
                    o[2] = ctx.emitterPosition[2];
                    o[3] = 0.0f;
                    break;
                case OpUser: splat(o, op.user >= 0 && ctx.userValues ? ctx.userValues[op.user] : 0.0f); break;
                case OpConstant: std::copy(op.constant, op.constant + 4, o); break;
                case OpAdd: for (int k = 0; k < 4; ++k) o[k] = a[k] + b[k]; break;
                case OpSubtract: for (int k = 0; k < 4; ++k) o[k] = a[k] - b[k]; break;
                case OpMultiply: for (int k = 0; k < 4; ++k) o[k] = a[k] * b[k]; break;
                case OpDivide: for (int k = 0; k < 4; ++k) o[k] = std::fabs(b[k]) > 1e-12f ? a[k] / b[k] : 0.0f; break;
                case OpMin: for (int k = 0; k < 4; ++k) o[k] = std::min(a[k], b[k]); break;
                case OpMax: for (int k = 0; k < 4; ++k) o[k] = std::max(a[k], b[k]); break;
                case OpPower: for (int k = 0; k < 4; ++k) o[k] = std::pow(std::max(a[k], 0.0f), b[k]); break;
                case OpLerp: for (int k = 0; k < 4; ++k) o[k] = a[k] + (b[k] - a[k]) * c[k]; break;
                case OpClamp: for (int k = 0; k < 4; ++k) o[k] = std::min(std::max(a[k], b[k]), c[k]); break;
                case OpSaturate: for (int k = 0; k < 4; ++k) o[k] = std::min(std::max(a[k], 0.0f), 1.0f); break;
                case OpAbs: for (int k = 0; k < 4; ++k) o[k] = std::fabs(a[k]); break;
                case OpNegate: for (int k = 0; k < 4; ++k) o[k] = -a[k]; break;
                case OpOneMinus: for (int k = 0; k < 4; ++k) o[k] = 1.0f - a[k]; break;
                case OpSin: for (int k = 0; k < 4; ++k) o[k] = std::sin(a[k]); break;
                case OpCos: for (int k = 0; k < 4; ++k) o[k] = std::cos(a[k]); break;
                case OpFrac: for (int k = 0; k < 4; ++k) o[k] = a[k] - std::floor(a[k]); break;
                case OpFloor: for (int k = 0; k < 4; ++k) o[k] = std::floor(a[k]); break;
                case OpSqrt: for (int k = 0; k < 4; ++k) o[k] = std::sqrt(std::max(a[k], 0.0f)); break;
                case OpStep: for (int k = 0; k < 4; ++k) o[k] = b[k] >= a[k] ? 1.0f : 0.0f; break;
                case OpSmoothstep:
                    for (int k = 0; k < 4; ++k)
                    {
                        const float span = b[k] - a[k];
                        float t = std::fabs(span) > 1e-12f ? (c[k] - a[k]) / span : 0.0f;
                        t = std::min(std::max(t, 0.0f), 1.0f);
                        o[k] = t * t * (3.0f - 2.0f * t);
                    }
                    break;
                case OpRemap:
                {
                    const float* d = fetch(op.in[3], lane);
                    const float* e = fetch(op.in[4], lane);
                    for (int k = 0; k < 4; ++k)
                    {
                        const float span = c[k] - b[k];
                        const float t = std::fabs(span) > 1e-12f ? (a[k] - b[k]) / span : 0.0f;
                        o[k] = d[k] + (e[k] - d[k]) * t;
                    }
                    break;
                }
                case OpSine:
                {
                    const float* d = fetch(op.in[3], lane);
                    float v = std::sin(6.2831853f * (a[0] * b[0] + c[0]));
                    if (op.mode == 1)
                        v = v * 0.5f + 0.5f;
                    splat(o, v * d[0]);
                    break;
                }
                case OpLength: splat(o, len(a, op.in[0].dim)); break;
                case OpNormalize:
                {
                    const float l = len(a, op.dim);
                    for (int k = 0; k < 4; ++k)
                        o[k] = k < op.dim && l > 1e-12f ? a[k] / l : 0.0f;
                    if (op.dim == 1)
                        splat(o, o[0]);
                    break;
                }
                case OpDot:
                {
                    const int dim = std::max(op.in[0].dim, op.in[1].dim);
                    float sum = 0.0f;
                    for (int k = 0; k < dim; ++k)
                        sum += a[k] * b[k];
                    splat(o, sum);
                    break;
                }
                case OpCross:
                    o[0] = a[1] * b[2] - a[2] * b[1];
                    o[1] = a[2] * b[0] - a[0] * b[2];
                    o[2] = a[0] * b[1] - a[1] * b[0];
                    o[3] = 0.0f;
                    break;
                case OpDistance:
                {
                    const int dim = std::max(op.in[0].dim, op.in[1].dim);
                    float d[4] = {};
                    for (int k = 0; k < 4; ++k)
                        d[k] = a[k] - b[k];
                    splat(o, len(d, dim));
                    break;
                }
                case OpMake3:
                    o[0] = a[0];
                    o[1] = b[0];
                    o[2] = c[0];
                    o[3] = 0.0f;
                    break;
                case OpMake4:
                    o[0] = a[0];
                    o[1] = b[0];
                    o[2] = c[0];
                    o[3] = fetch(op.in[3], lane)[0];
                    break;
                case OpSplit:
                    for (int k = 0; k < 4; ++k)
                        splat(slotPtr(op.out + k, lane), a[k]);
                    break;
                case OpRandom:
                {
                    uint32_t key = op.seed;
                    if (op.mode == 0 && ctx.streams)
                        key ^= ParticleHash(static_cast<uint32_t>(ctx.streams[ParticleAttr::Id][particle]) + 0x9E3779B9u);
                    else
                        key ^= ParticleHash(ctx.frameSeed + static_cast<uint32_t>(particle) * 7919u);
                    for (int k = 0; k < 4; ++k)
                        o[k] = a[k] + (b[k] - a[k]) * ParticleHash01(key + static_cast<uint32_t>(k) * 0x68E31DA4u);
                    if (op.dim == 1)
                        splat(o, o[0]);
                    break;
                }
                case OpNoise:
                    splat(o, ParticleValueNoise(a[0] * b[0], a[1] * b[0], a[2] * b[0]));
                    break;
                case OpVectorNoise:
                    ParticleVectorNoise(a[0] * b[0], a[1] * b[0], a[2] * b[0], o);
                    o[3] = 0.0f;
                    break;
                case OpCurve: splat(o, EvaluateParticleCurve(op.curve, a[0])); break;
                case OpGradient: EvaluateParticleGradient(op.gradient, a[0], o); break;
                default: splat(o, 0.0f); break;
                }
            }
        }

        for (int lane = 0; lane < lanes; ++lane)
        {
            const float* r = fetch(result, lane);
            float* o = out + static_cast<size_t>(chunk + static_cast<uint32_t>(lane)) * 4;
            std::copy(r, r + 4, o);
        }
    }
}

#include "particle_asset.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include "text_block_file.h"
#include "text_parse_utils.h"

namespace
{
    constexpr uint32_t kSpawnStage = 1u << static_cast<uint32_t>(ParticleStage::ParticleSpawn);
    constexpr uint32_t kUpdateStage = 1u << static_cast<uint32_t>(ParticleStage::ParticleUpdate);
    constexpr uint32_t kEmitterStage = 1u << static_cast<uint32_t>(ParticleStage::EmitterUpdate);

    ParticleParamDef V(const char* name, int dim, const char* def, const char* tooltip = "", bool degrees = false)
    {
        ParticleParamDef p;
        p.name = name;
        p.kind = ParticleParamKind::Value;
        p.dim = dim;
        p.defaultText = def;
        p.tooltip = tooltip;
        p.degrees = degrees;
        return p;
    }

    ParticleParamDef E(const char* name, const char* options, const char* def, const char* tooltip = "")
    {
        ParticleParamDef p;
        p.name = name;
        p.kind = ParticleParamKind::Enum;
        p.options = options;
        p.defaultText = def;
        p.tooltip = tooltip;
        return p;
    }

    ParticleParamDef B(const char* name, bool def, const char* tooltip = "")
    {
        ParticleParamDef p;
        p.name = name;
        p.kind = ParticleParamKind::Bool;
        p.defaultText = def ? "true" : "false";
        p.tooltip = tooltip;
        return p;
    }

    ParticleParamDef I(const char* name, int def, const char* tooltip = "")
    {
        ParticleParamDef p;
        p.name = name;
        p.kind = ParticleParamKind::Int;
        p.defaultText = std::to_string(def);
        p.tooltip = tooltip;
        return p;
    }

    ParticleParamDef A(const char* name, const char* extensions, const char* tooltip = "")
    {
        ParticleParamDef p;
        p.name = name;
        p.kind = ParticleParamKind::Asset;
        p.options = extensions;
        p.tooltip = tooltip;
        return p;
    }

    ParticleParamDef T(const char* name, const char* def, const char* tooltip = "")
    {
        ParticleParamDef p;
        p.name = name;
        p.kind = ParticleParamKind::Text;
        p.defaultText = def;
        p.tooltip = tooltip;
        return p;
    }

    ParticleModuleDef M(const char* type, const char* title, const char* category, uint32_t stages, const char* description,
        std::vector<ParticleParamDef> params)
    {
        ParticleModuleDef d;
        d.type = type;
        d.title = title;
        d.category = category;
        d.stageMask = stages;
        d.description = description;
        d.params = std::move(params);
        return d;
    }

    std::vector<ParticleModuleDef> BuildModuleCatalog()
    {
        std::vector<ParticleModuleDef> c;
        c.push_back(M("SpawnRate", "Spawn Rate", "Spawn", kEmitterStage, "Spawns a steady number of particles per second.",
            { V("Rate", 1, "20", "Particles per second") }));
        c.push_back(M("SpawnBurst", "Spawn Burst", "Spawn", kEmitterStage, "Spawns a number of particles at once.",
            { V("Time", 1, "0", "Seconds into the loop"), V("Count", 1, "20"),
                V("Interval", 1, "0", "Seconds between repeats, 0 = once per loop"), I("Repeat", 1, "Bursts per loop when Interval > 0") }));
        c.push_back(M("SpawnPerUnit", "Spawn Per Unit", "Spawn", kEmitterStage, "Spawns particles along the path the system moves (trails).",
            { V("PerUnit", 1, "4", "Particles per meter moved"), I("MaxPerFrame", 200) }));

        c.push_back(M("InitializeParticle", "Initialize Particle", "Initialize", kSpawnStage, "Lifetime, color, size and mass of new particles.",
            { V("Lifetime", 1, "random uniform 1.5 ; 2.5", "Seconds"), V("Color", 4, "1 1 1 1"), V("SpriteSize", 2, "0.2 0.2", "Meters"),
                V("SpriteRotation", 1, "0", "Degrees", true), V("Mass", 1, "1"), V("MeshScale", 3, "1 1 1") }));
        c.push_back(M("ShapeLocation", "Shape Location", "Location", kSpawnStage, "Places new particles in a shape around the system.",
            { E("Shape", "Point|Sphere|Box|Cylinder|Cone|Ring", "Sphere"), V("Radius", 1, "0.5"), V("BoxSize", 3, "1 1 1"),
                V("Height", 1, "1", "Cylinder and cone height"), V("ConeAngle", 1, "30", "Degrees", true),
                B("SurfaceOnly", false), V("Offset", 3, "0 0 0") }));
        c.push_back(M("AddVelocity", "Add Velocity", "Velocity", kSpawnStage | kUpdateStage, "Adds velocity to particles.",
            { E("Mode", "Linear|FromCenter|Cone", "Linear"), V("Velocity", 3, "0 2 0", "Linear: meters per second"),
                V("Speed", 1, "2", "FromCenter and Cone"), V("ConeAxis", 3, "0 1 0"), V("ConeAngle", 1, "25", "Degrees", true) }));
        c.push_back(M("InheritVelocity", "Inherit Velocity", "Velocity", kSpawnStage, "Adds the system's own movement to new particles.",
            { V("Scale", 1, "1") }));
        c.push_back(M("InitializeRotation", "Initialize Rotation", "Initialize", kSpawnStage, "Spin of sprites and meshes.",
            { V("SpriteRotationRate", 1, "0", "Degrees per second", true), V("MeshRotation", 3, "0 0 0", "Degrees", true),
                V("MeshAngularVelocity", 3, "0 0 0", "Degrees per second", true) }));

        c.push_back(M("Gravity", "Gravity Force", "Forces", kUpdateStage, "Constant acceleration in world space.",
            { V("Acceleration", 3, "0 -9.8 0") }));
        c.push_back(M("Drag", "Drag", "Forces", kUpdateStage, "Slows particles down.",
            { V("Drag", 1, "1", "Per second") }));
        c.push_back(M("Force", "Linear Force", "Forces", kUpdateStage, "Constant force (wind), divided by mass.",
            { V("Force", 3, "1 0 0"), E("Space", "World|Local", "World") }));
        c.push_back(M("CurlNoise", "Curl Noise Force", "Forces", kUpdateStage, "Swirling turbulence.",
            { V("Strength", 1, "2"), V("Frequency", 1, "0.5"), V("Pan", 3, "0 0.3 0", "Scrolls the noise field per second") }));
        c.push_back(M("Vortex", "Vortex Force", "Forces", kUpdateStage, "Spins particles around an axis through the system.",
            { V("Axis", 3, "0 1 0"), V("Strength", 1, "3"), V("Pull", 1, "0", "Towards the axis") }));
        c.push_back(M("PointAttraction", "Point Attraction", "Forces", kUpdateStage, "Pulls particles to a point.",
            { V("Position", 3, "0 0 0", "Relative to the system"), V("Strength", 1, "5"), V("Radius", 1, "3"),
                V("KillRadius", 1, "0", "Particles closer than this die") }));
        c.push_back(M("LimitSpeed", "Limit Speed", "Forces", kUpdateStage, "Caps the speed.",
            { V("MaxSpeed", 1, "10") }));
        c.push_back(M("ScaleColor", "Scale Color", "Color", kUpdateStage, "Spawn color multiplied by a value (usually a gradient over life).",
            { V("Scale", 4, "gradient NormalizedAge ; 0 1 1 1 1 ; 1 1 1 1 0") }));
        c.push_back(M("ScaleSize", "Scale Sprite Size", "Size", kUpdateStage, "Spawn size multiplied by a value (usually a curve over life).",
            { V("Scale", 2, "curve NormalizedAge 1 1 ; 0 1 ; 1 1") }));
        c.push_back(M("ScaleMeshSize", "Scale Mesh Size", "Size", kUpdateStage, "Mesh scale over life.",
            { V("Scale", 3, "1 1 1") }));
        c.push_back(M("SubUVAnimation", "Sub UV Animation", "Sprite", kUpdateStage, "Flipbook frame (the sprite renderer's SubImages grid).",
            { E("Mode", "OverLife|FrameRate|Random", "OverLife"), V("Frames", 1, "16"), V("FrameRate", 1, "15"), V("StartFrame", 1, "0") }));
        c.push_back(M("Collision", "Collision", "Collision", kUpdateStage, "Bounces off the world or a ground plane.",
            { E("Mode", "World|GroundPlane", "World"), V("PlaneHeight", 1, "0", "GroundPlane: world height"),
                V("Radius", 1, "0.05"), V("Restitution", 1, "0.4"), V("Friction", 1, "0.2"), E("Response", "Bounce|Stop|Kill", "Bounce") }));
        c.push_back(M("KillVolume", "Kill Volume", "Kill", kUpdateStage, "Kills particles inside (or outside) a shape.",
            { E("Shape", "Sphere|Box", "Sphere"), E("Space", "Local|World", "Local"), V("Center", 3, "0 0 0"), V("Radius", 1, "5"),
                V("Size", 3, "10 10 10"), B("Invert", false, "Kill outside instead") }));
        c.push_back(M("KillIf", "Kill If", "Kill", kUpdateStage, "Kills particles whose Condition is above 0.5 (wire a graph).",
            { V("Condition", 1, "0") }));
        c.push_back(M("SetAttribute", "Set Attribute", "Custom", kSpawnStage | kUpdateStage,
            "Writes any attribute (built in or custom) from a value or graph.",
            { T("Attribute", "Color", "Position, Velocity, Color, SpriteSize, SpriteRotation, Mass, MeshScale, Lifetime, or a custom attribute"),
                V("Value", 4, "1 1 1 1") }));
        return c;
    }

    std::vector<ParticleModuleDef> BuildRendererCatalog()
    {
        std::vector<ParticleModuleDef> c;
        c.push_back(M("Sprite", "Sprite Renderer", "Renderers", 0, "Camera-facing quads.",
            { A("Material", "mat", "Base texture and color; empty = white"), E("Blend", "Additive|Alpha|Premultiplied", "Additive"),
                E("Alignment", "FaceCamera|Velocity|Axis", "FaceCamera"), V("Axis", 3, "0 1 0", "Alignment Axis: the quad's normal"),
                T("SubImages", "1 1", "Flipbook columns rows"), E("Sort", "None|BackToFront", "None"), B("Lit", false),
                V("Emissive", 1, "1", "Color multiplier (HDR)"), V("VelocityStretch", 1, "0", "Velocity alignment: extra length per m/s"),
                I("SortOrder", 0, "Draw order between emitters: higher draws later (on top). Ties: blended first, additive last, then far to near") }));
        c.push_back(M("Ribbon", "Ribbon Renderer", "Renderers", 0, "A strip through the particles in spawn order (trails).",
            { A("Material", "mat"), E("Blend", "Additive|Alpha|Premultiplied", "Additive"), V("Width", 1, "0.2"),
                E("TextureMode", "Stretch|Tile", "Stretch"), V("TileLength", 1, "1"), B("Lit", false), V("Emissive", 1, "1"),
                I("SortOrder", 0, "Draw order between emitters: higher draws later (on top)") }));
        c.push_back(M("Mesh", "Mesh Renderer", "Renderers", 0, "A model per particle.",
            { A("Model", "kmodel|kmesh|obj"), A("Material", "mat", "Overrides the model's material"),
                E("Orientation", "Rotation|Velocity", "Rotation") }));
        c.push_back(M("Light", "Light Renderer", "Renderers", 0, "Every particle becomes a point light.",
            { V("Radius", 1, "3"), V("Intensity", 1, "1"), B("UseParticleColor", true), V("Color", 3, "1 1 1"),
                V("Fraction", 1, "1", "Share of particles that emit light (0-1)"), I("MaxLights", 128) }));
        return c;
    }

    std::string FormatFloat(float v)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(v));
        return buffer;
    }

    std::vector<std::string> SplitGroups(const std::string& text)
    {
        std::vector<std::string> groups;
        std::string current;
        for (char ch : text)
        {
            if (ch == ';')
            {
                groups.push_back(textparse::Trim(current));
                current.clear();
            }
            else
                current.push_back(ch);
        }
        groups.push_back(textparse::Trim(current));
        return groups;
    }

    std::vector<std::string> Tokens(const std::string& text)
    {
        std::vector<std::string> out;
        std::istringstream stream(text);
        std::string token;
        while (stream >> token)
            out.push_back(token);
        return out;
    }

    bool ParseFloatToken(const std::string& token, float& out)
    {
        char* end = nullptr;
        out = std::strtof(token.c_str(), &end);
        return end != token.c_str();
    }

    // Fills `out[0..dim)` from numbers; one number fills every component.
    void ReadComponents(const std::vector<std::string>& tokens, size_t first, int dim, float out[4])
    {
        float values[4] = {};
        int count = 0;
        for (size_t i = first; i < tokens.size() && count < 4; ++i)
        {
            if (ParseFloatToken(tokens[i], values[count]))
                ++count;
        }
        for (int i = 0; i < 4; ++i)
            out[i] = count == 0 ? 0.0f : (count == 1 ? values[0] : (i < count ? values[i] : 0.0f));
        (void)dim;
    }

    bool ParseCurveInput(const std::string& text, ParticleCurveInput& out)
    {
        for (int i = 0; i <= static_cast<int>(ParticleCurveInput::NormalizedLoopTime); ++i)
        {
            if (text == ParticleCurveInputName(static_cast<ParticleCurveInput>(i)))
            {
                out = static_cast<ParticleCurveInput>(i);
                return true;
            }
        }
        return false;
    }

    int ParseInt(const std::string& value, int fallback)
    {
        const std::string cleaned = textparse::ParseString(value);
        char* end = nullptr;
        const long parsed = std::strtol(cleaned.c_str(), &end, 10);
        return end == cleaned.c_str() ? fallback : static_cast<int>(parsed);
    }

    void WriteKey(std::ostringstream& out, const char* key, const std::string& value)
    {
        out << "    " << key << " = " << value << "\n";
    }

    void WriteModuleParams(std::ostringstream& out, const ParticleModuleData& module, const ParticleModuleDef* def)
    {
        for (const ParticleParam& param : module.params)
        {
            const ParticleParamDef* paramDef = def ? def->FindParam(param.name) : nullptr;
            const std::string key = "Param." + param.name;
            if (paramDef && paramDef->kind == ParticleParamKind::Value)
                WriteKey(out, key.c_str(), textparse::Quote(FormatParticleValue(param.value, paramDef->dim)));
            else
                WriteKey(out, key.c_str(), textparse::Quote(param.text));
        }
    }

    void ReadModuleParams(const TextBlock& block, ParticleModuleData& module, const ParticleModuleDef* def)
    {
        for (const TextBlockProperty& property : block.properties)
        {
            if (property.key.rfind("Param.", 0) != 0)
                continue;
            ParticleParam param;
            param.name = property.key.substr(6);
            const std::string text = textparse::ParseString(property.value);
            const ParticleParamDef* paramDef = def ? def->FindParam(param.name) : nullptr;
            if (paramDef && paramDef->kind == ParticleParamKind::Value)
            {
                if (!ParseParticleValue(text, paramDef->dim, param.value))
                    ParseParticleValue(paramDef->defaultText, paramDef->dim, param.value);
            }
            else
                param.text = text;
            module.params.push_back(std::move(param));
        }
        if (def)
            ApplyParticleDefaults(module, *def);
    }

    std::string FloatList(const std::vector<float>& values)
    {
        std::string out;
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i)
                out += ' ';
            out += FormatFloat(values[i]);
        }
        return out;
    }
}

const char* ParticleStageName(ParticleStage stage)
{
    switch (stage)
    {
    case ParticleStage::EmitterUpdate: return "EmitterUpdate";
    case ParticleStage::ParticleSpawn: return "ParticleSpawn";
    default: return "ParticleUpdate";
    }
}

bool ParseParticleStage(const std::string& text, ParticleStage& out)
{
    for (int i = 0; i < kParticleStageCount; ++i)
    {
        if (text == ParticleStageName(static_cast<ParticleStage>(i)))
        {
            out = static_cast<ParticleStage>(i);
            return true;
        }
    }
    return false;
}

const char* ParticleCurveInputName(ParticleCurveInput input)
{
    switch (input)
    {
    case ParticleCurveInput::Age: return "Age";
    case ParticleCurveInput::Speed: return "Speed";
    case ParticleCurveInput::Random: return "Random";
    case ParticleCurveInput::EmitterTime: return "EmitterTime";
    case ParticleCurveInput::NormalizedLoopTime: return "NormalizedLoopTime";
    default: return "NormalizedAge";
    }
}

ParticleValue ParticleValue::Constant(float x, float y, float z, float w)
{
    ParticleValue v;
    v.a[0] = x;
    v.a[1] = y;
    v.a[2] = z;
    v.a[3] = w;
    return v;
}

float EvaluateParticleCurve(const std::vector<ParticleCurveKey>& keys, float x)
{
    if (keys.empty())
        return 1.0f;
    if (x <= keys.front().time)
        return keys.front().value;
    if (x >= keys.back().time)
        return keys.back().value;
    for (size_t i = 1; i < keys.size(); ++i)
    {
        if (x <= keys[i].time)
        {
            const ParticleCurveKey& k0 = keys[i - 1];
            const ParticleCurveKey& k1 = keys[i];
            const float span = k1.time - k0.time;
            float t = span > 1e-6f ? (x - k0.time) / span : 1.0f;
            t = t * t * (3.0f - 2.0f * t); // smooth between keys
            return k0.value + (k1.value - k0.value) * t;
        }
    }
    return keys.back().value;
}

void EvaluateParticleGradient(const std::vector<ParticleGradientKey>& keys, float x, float out[4])
{
    if (keys.empty())
    {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    const ParticleGradientKey* k0 = &keys.front();
    const ParticleGradientKey* k1 = &keys.front();
    if (x >= keys.back().time)
        k0 = k1 = &keys.back();
    else if (x > keys.front().time)
    {
        for (size_t i = 1; i < keys.size(); ++i)
        {
            if (x <= keys[i].time)
            {
                k0 = &keys[i - 1];
                k1 = &keys[i];
                break;
            }
        }
    }
    const float span = k1->time - k0->time;
    const float t = span > 1e-6f ? (x - k0->time) / span : 0.0f;
    for (int i = 0; i < 4; ++i)
        out[i] = k0->color[i] + (k1->color[i] - k0->color[i]) * t;
}

std::string FormatParticleValue(const ParticleValue& value, int dim)
{
    dim = std::clamp(dim, 1, 4);
    auto components = [dim](const float* v)
    {
        std::string s;
        for (int i = 0; i < dim; ++i)
        {
            if (i)
                s += ' ';
            s += FormatFloat(v[i]);
        }
        return s;
    };

    switch (value.mode)
    {
    case ParticleValueMode::Random:
        return std::string("random ") + (value.uniform ? "uniform " : "separate ") + components(value.a) + " ; " + components(value.b);
    case ParticleValueMode::Curve:
    {
        std::string s = std::string("curve ") + ParticleCurveInputName(value.input) + " " + components(value.a);
        for (const ParticleCurveKey& key : value.curve)
            s += " ; " + FormatFloat(key.time) + " " + FormatFloat(key.value);
        return s;
    }
    case ParticleValueMode::Gradient:
    {
        std::string s = std::string("gradient ") + ParticleCurveInputName(value.input);
        for (const ParticleGradientKey& key : value.gradient)
        {
            s += " ; " + FormatFloat(key.time);
            for (float c : key.color)
                s += " " + FormatFloat(c);
        }
        return s;
    }
    case ParticleValueMode::Graph:
        return "graph " + std::to_string(value.graph);
    default:
        return components(value.a);
    }
}

bool ParseParticleValue(const std::string& text, int dim, ParticleValue& out)
{
    out = ParticleValue();
    const std::vector<std::string> groups = SplitGroups(textparse::Trim(text));
    const std::vector<std::string> head = Tokens(groups.front());
    if (head.empty())
        return false;

    const std::string& kind = head.front();
    float probe = 0.0f;
    if (ParseFloatToken(kind, probe) || kind == "const")
    {
        out.mode = ParticleValueMode::Constant;
        ReadComponents(head, kind == "const" ? 1 : 0, dim, out.a);
        return true;
    }
    if (kind == "random")
    {
        out.mode = ParticleValueMode::Random;
        size_t first = 1;
        if (head.size() > 1 && (head[1] == "uniform" || head[1] == "separate"))
        {
            out.uniform = head[1] == "uniform";
            first = 2;
        }
        ReadComponents(head, first, dim, out.a);
        if (groups.size() < 2)
            return false;
        ReadComponents(Tokens(groups[1]), 0, dim, out.b);
        return true;
    }
    if (kind == "curve")
    {
        out.mode = ParticleValueMode::Curve;
        size_t first = 1;
        if (head.size() > 1 && ParseCurveInput(head[1], out.input))
            first = 2;
        out.a[0] = out.a[1] = out.a[2] = out.a[3] = 1.0f;
        if (head.size() > first)
            ReadComponents(head, first, dim, out.a);
        for (size_t g = 1; g < groups.size(); ++g)
        {
            const std::vector<std::string> key = Tokens(groups[g]);
            ParticleCurveKey k;
            if (key.size() >= 2 && ParseFloatToken(key[0], k.time) && ParseFloatToken(key[1], k.value))
                out.curve.push_back(k);
        }
        std::sort(out.curve.begin(), out.curve.end(), [](const ParticleCurveKey& a, const ParticleCurveKey& b) { return a.time < b.time; });
        return true;
    }
    if (kind == "gradient")
    {
        out.mode = ParticleValueMode::Gradient;
        if (head.size() > 1)
            ParseCurveInput(head[1], out.input);
        for (size_t g = 1; g < groups.size(); ++g)
        {
            const std::vector<std::string> key = Tokens(groups[g]);
            ParticleGradientKey k;
            if (key.empty() || !ParseFloatToken(key[0], k.time))
                continue;
            for (int c = 0; c < 4 && c + 1 < static_cast<int>(key.size()); ++c)
                ParseFloatToken(key[static_cast<size_t>(c) + 1], k.color[c]);
            out.gradient.push_back(k);
        }
        std::sort(out.gradient.begin(), out.gradient.end(),
            [](const ParticleGradientKey& a, const ParticleGradientKey& b) { return a.time < b.time; });
        return true;
    }
    if (kind == "graph")
    {
        out.mode = ParticleValueMode::Graph;
        out.graph = head.size() > 1 ? std::atoi(head[1].c_str()) : 0;
        return true;
    }
    return false;
}

const ParticleParamDef* ParticleModuleDef::FindParam(const std::string& name) const
{
    for (const ParticleParamDef& p : params)
    {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

const std::vector<ParticleModuleDef>& ParticleModuleCatalog()
{
    static const std::vector<ParticleModuleDef> catalog = BuildModuleCatalog();
    return catalog;
}

const std::vector<ParticleModuleDef>& ParticleRendererCatalog()
{
    static const std::vector<ParticleModuleDef> catalog = BuildRendererCatalog();
    return catalog;
}

const ParticleModuleDef* FindParticleModuleDef(const std::string& type)
{
    for (const ParticleModuleDef& d : ParticleModuleCatalog())
    {
        if (d.type == type)
            return &d;
    }
    return nullptr;
}

const ParticleModuleDef* FindParticleRendererDef(const std::string& type)
{
    for (const ParticleModuleDef& d : ParticleRendererCatalog())
    {
        if (d.type == type)
            return &d;
    }
    return nullptr;
}

const ParticleParam* ParticleModuleData::Find(const std::string& name) const
{
    for (const ParticleParam& p : params)
    {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

ParticleParam* ParticleModuleData::Find(const std::string& name)
{
    for (ParticleParam& p : params)
    {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

void ApplyParticleDefaults(ParticleModuleData& module, const ParticleModuleDef& def)
{
    std::vector<ParticleParam> ordered;
    for (const ParticleParamDef& paramDef : def.params)
    {
        if (const ParticleParam* existing = module.Find(paramDef.name))
        {
            ordered.push_back(*existing);
            continue;
        }
        ParticleParam param;
        param.name = paramDef.name;
        if (paramDef.kind == ParticleParamKind::Value)
            ParseParticleValue(paramDef.defaultText, paramDef.dim, param.value);
        else
            param.text = paramDef.defaultText;
        ordered.push_back(std::move(param));
    }
    for (const ParticleParam& param : module.params)
    {
        if (!def.FindParam(param.name))
            ordered.push_back(param);
    }
    module.params = std::move(ordered);
}

ParticleModuleData MakeParticleModule(const std::string& type, ParticleStage stage)
{
    ParticleModuleData module;
    module.type = type;
    module.stage = stage;
    if (const ParticleModuleDef* def = FindParticleModuleDef(type))
        ApplyParticleDefaults(module, *def);
    return module;
}

ParticleRendererData MakeParticleRenderer(const std::string& type)
{
    ParticleRendererData renderer;
    renderer.type = type;
    if (const ParticleModuleDef* def = FindParticleRendererDef(type))
        ApplyParticleDefaults(renderer, *def);
    return renderer;
}

const std::string* ParticleGraphNode::FindParam(const std::string& name) const
{
    for (const auto& p : params)
    {
        if (p.first == name)
            return &p.second;
    }
    return nullptr;
}

const std::vector<float>* ParticleGraphNode::FindInputDefault(int port) const
{
    for (const auto& d : inputDefaults)
    {
        if (d.first == port)
            return &d.second;
    }
    return nullptr;
}

const ParticleGraphData* ParticleEmitterData::FindGraph(int id) const
{
    for (const ParticleGraphData& g : graphs)
    {
        if (g.id == id)
            return &g;
    }
    return nullptr;
}

int ParticleEmitterData::NextGraphId() const
{
    int id = 1;
    for (const ParticleGraphData& g : graphs)
        id = std::max(id, g.id + 1);
    return id;
}

bool LoadParticleSystemFile(const char* path, ParticleSystemData& out, std::string* outError)
{
    out = {};
    std::vector<TextBlock> blocks;
    if (!LoadTextBlockFile(path, blocks, outError))
        return false;

    auto emitterOf = [&](const TextBlock& block) -> ParticleEmitterData*
    {
        const std::string* index = FindTextBlockProperty(block, "Emitter");
        const int i = index ? ParseInt(*index, -1) : -1;
        return i >= 0 && i < static_cast<int>(out.emitters.size()) ? &out.emitters[static_cast<size_t>(i)] : nullptr;
    };
    auto graphOf = [&](const TextBlock& block) -> ParticleGraphData*
    {
        ParticleEmitterData* emitter = emitterOf(block);
        const std::string* id = FindTextBlockProperty(block, "Graph");
        if (!emitter || !id)
            return nullptr;
        const int graphId = ParseInt(*id, -1);
        for (ParticleGraphData& g : emitter->graphs)
        {
            if (g.id == graphId)
                return &g;
        }
        return nullptr;
    };

    for (const TextBlock& block : blocks)
    {
        if (block.type == "System")
        {
            if (const std::string* v = FindTextBlockProperty(block, "Warmup"))
                out.warmup = textparse::ParseFloat(*v, 0.0f);
        }
        else if (block.type == "UserParam")
        {
            ParticleUserParam param;
            if (const std::string* v = FindTextBlockProperty(block, "Name"))
                param.name = textparse::ParseString(*v);
            if (const std::string* v = FindTextBlockProperty(block, "Value"))
                param.value = textparse::ParseFloat(*v, 0.0f);
            if (!param.name.empty())
                out.userParams.push_back(param);
        }
        else if (block.type == "Emitter")
        {
            ParticleEmitterData e;
            for (const TextBlockProperty& p : block.properties)
            {
                if (p.key == "Name") e.name = textparse::ParseString(p.value);
                else if (p.key == "Enabled") e.enabled = textparse::ParseBool(p.value, true);
                else if (p.key == "LocalSpace") e.localSpace = textparse::ParseBool(p.value, false);
                else if (p.key == "MaxParticles") e.maxParticles = static_cast<uint32_t>(std::max(1, ParseInt(p.value, 1000)));
                else if (p.key == "Duration") e.duration = std::max(0.0f, textparse::ParseFloat(p.value, 0.0f));
                else if (p.key == "Loop") e.loop = textparse::ParseBool(p.value, true);
                else if (p.key == "LoopCount") e.loopCount = static_cast<uint32_t>(std::max(0, ParseInt(p.value, 0)));
                else if (p.key == "Delay") e.delay = std::max(0.0f, textparse::ParseFloat(p.value, 0.0f));
                else if (p.key == "Seed") e.seed = static_cast<uint32_t>(std::max(0, ParseInt(p.value, 0)));
                else if (p.key == "CustomAttributes") e.customAttributes = textparse::SplitList(p.value);
            }
            out.emitters.push_back(std::move(e));
        }
        else if (block.type == "Module" || block.type == "Renderer")
        {
            ParticleEmitterData* emitter = emitterOf(block);
            const std::string* type = FindTextBlockProperty(block, "Type");
            if (!emitter || !type)
                continue;
            ParticleModuleData module;
            module.type = textparse::ParseString(*type);
            if (const std::string* v = FindTextBlockProperty(block, "Enabled"))
                module.enabled = textparse::ParseBool(*v, true);
            if (block.type == "Module")
            {
                if (const std::string* v = FindTextBlockProperty(block, "Stage"))
                    ParseParticleStage(textparse::ParseString(*v), module.stage);
                ReadModuleParams(block, module, FindParticleModuleDef(module.type));
                emitter->modules.push_back(std::move(module));
            }
            else
            {
                ReadModuleParams(block, module, FindParticleRendererDef(module.type));
                emitter->renderers.push_back(std::move(module));
            }
        }
        else if (block.type == "Graph")
        {
            ParticleEmitterData* emitter = emitterOf(block);
            const std::string* id = FindTextBlockProperty(block, "Id");
            if (!emitter || !id)
                continue;
            ParticleGraphData graph;
            graph.id = ParseInt(*id, 0);
            emitter->graphs.push_back(std::move(graph));
        }
        else if (block.type == "GraphNode")
        {
            ParticleGraphData* graph = graphOf(block);
            if (!graph)
                continue;
            ParticleGraphNode node;
            for (const TextBlockProperty& p : block.properties)
            {
                if (p.key == "Id") node.id = ParseInt(p.value, 0);
                else if (p.key == "Type") node.type = textparse::ParseString(p.value);
                else if (p.key == "Pos")
                {
                    const Vector2 pos = textparse::ParseVector2(p.value, {});
                    node.x = pos.x;
                    node.y = pos.y;
                }
                else if (p.key.rfind("Param.", 0) == 0)
                    node.params.emplace_back(p.key.substr(6), textparse::ParseString(p.value));
                else if (p.key.rfind("In.", 0) == 0)
                {
                    std::vector<float> values;
                    for (const std::string& token : Tokens(textparse::ParseString(p.value)))
                    {
                        float f = 0.0f;
                        if (ParseFloatToken(token, f))
                            values.push_back(f);
                    }
                    node.inputDefaults.emplace_back(ParseInt(p.key.substr(3), 0), std::move(values));
                }
            }
            graph->nodes.push_back(std::move(node));
        }
        else if (block.type == "GraphLink")
        {
            ParticleGraphData* graph = graphOf(block);
            const std::string* from = FindTextBlockProperty(block, "From");
            const std::string* to = FindTextBlockProperty(block, "To");
            if (!graph || !from || !to)
                continue;
            const Vector2 a = textparse::ParseVector2(*from, { -1.0f, -1.0f });
            const Vector2 b = textparse::ParseVector2(*to, { -1.0f, -1.0f });
            graph->links.push_back({ static_cast<int>(a.x), static_cast<int>(a.y), static_cast<int>(b.x), static_cast<int>(b.y) });
        }
    }

    // Stage order, keeping the stack order inside each stage.
    for (ParticleEmitterData& e : out.emitters)
        std::stable_sort(e.modules.begin(), e.modules.end(),
            [](const ParticleModuleData& a, const ParticleModuleData& b) { return a.stage < b.stage; });
    return true;
}

std::string WriteParticleSystemText(const ParticleSystemData& system)
{
    std::ostringstream out;
    out << "// KES particle system\n\nSystem\n{\n";
    WriteKey(out, "Version", "1");
    WriteKey(out, "Warmup", FormatFloat(system.warmup));
    out << "}\n";

    for (const ParticleUserParam& param : system.userParams)
    {
        out << "\nUserParam\n{\n";
        WriteKey(out, "Name", textparse::Quote(param.name));
        WriteKey(out, "Value", FormatFloat(param.value));
        out << "}\n";
    }

    for (size_t ei = 0; ei < system.emitters.size(); ++ei)
    {
        const ParticleEmitterData& e = system.emitters[ei];
        const std::string index = std::to_string(ei);
        out << "\nEmitter\n{\n";
        WriteKey(out, "Name", textparse::Quote(e.name));
        WriteKey(out, "Enabled", e.enabled ? "true" : "false");
        WriteKey(out, "LocalSpace", e.localSpace ? "true" : "false");
        WriteKey(out, "MaxParticles", std::to_string(e.maxParticles));
        WriteKey(out, "Duration", FormatFloat(e.duration));
        WriteKey(out, "Loop", e.loop ? "true" : "false");
        WriteKey(out, "LoopCount", std::to_string(e.loopCount));
        WriteKey(out, "Delay", FormatFloat(e.delay));
        WriteKey(out, "Seed", std::to_string(e.seed));
        if (!e.customAttributes.empty())
        {
            std::string list;
            for (const std::string& a : e.customAttributes)
                list += (list.empty() ? "" : ",") + a;
            WriteKey(out, "CustomAttributes", textparse::Quote(list));
        }
        out << "}\n";

        for (int stage = 0; stage < kParticleStageCount; ++stage)
        {
            for (const ParticleModuleData& m : e.modules)
            {
                if (static_cast<int>(m.stage) != stage)
                    continue;
                out << "\nModule\n{\n";
                WriteKey(out, "Emitter", index);
                WriteKey(out, "Stage", ParticleStageName(m.stage));
                WriteKey(out, "Type", m.type);
                WriteKey(out, "Enabled", m.enabled ? "true" : "false");
                WriteModuleParams(out, m, FindParticleModuleDef(m.type));
                out << "}\n";
            }
        }

        for (const ParticleRendererData& r : e.renderers)
        {
            out << "\nRenderer\n{\n";
            WriteKey(out, "Emitter", index);
            WriteKey(out, "Type", r.type);
            WriteKey(out, "Enabled", r.enabled ? "true" : "false");
            WriteModuleParams(out, r, FindParticleRendererDef(r.type));
            out << "}\n";
        }

        for (const ParticleGraphData& g : e.graphs)
        {
            out << "\nGraph\n{\n";
            WriteKey(out, "Emitter", index);
            WriteKey(out, "Id", std::to_string(g.id));
            out << "}\n";
            for (const ParticleGraphNode& n : g.nodes)
            {
                out << "GraphNode\n{\n";
                WriteKey(out, "Emitter", index);
                WriteKey(out, "Graph", std::to_string(g.id));
                WriteKey(out, "Id", std::to_string(n.id));
                WriteKey(out, "Type", n.type);
                WriteKey(out, "Pos", textparse::Quote(FormatFloat(n.x) + " " + FormatFloat(n.y)));
                for (const auto& p : n.params)
                    WriteKey(out, ("Param." + p.first).c_str(), textparse::Quote(p.second));
                for (const auto& d : n.inputDefaults)
                    WriteKey(out, ("In." + std::to_string(d.first)).c_str(), textparse::Quote(FloatList(d.second)));
                out << "}\n";
            }
            for (const ParticleGraphLink& l : g.links)
            {
                out << "GraphLink\n{\n";
                WriteKey(out, "Emitter", index);
                WriteKey(out, "Graph", std::to_string(g.id));
                WriteKey(out, "From", textparse::Quote(std::to_string(l.fromNode) + " " + std::to_string(l.fromPort)));
                WriteKey(out, "To", textparse::Quote(std::to_string(l.toNode) + " " + std::to_string(l.toPort)));
                out << "}\n";
            }
        }
    }
    return out.str();
}

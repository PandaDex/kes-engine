#include "particle_sim.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "text_parse_utils.h"

namespace
{
    constexpr float kDegToRad = 3.14159265f / 180.0f;
    constexpr float kTwoPi = 6.28318531f;
    constexpr int kScratchSlots = 10;

    float Rand(uint32_t id, uint32_t salt, uint32_t k)
    {
        return ParticleHash01(ParticleHash(id * 0x9E3779B9u + salt) ^ (k * 0x85EBCA6Bu + 0x27D4EB2Fu));
    }

    Vector3 RandomUnitVector(uint32_t id, uint32_t salt, uint32_t k)
    {
        const float z = Rand(id, salt, k) * 2.0f - 1.0f;
        const float a = Rand(id, salt, k + 1) * kTwoPi;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return { r * std::cos(a), z, r * std::sin(a) };
    }

    // Orthonormal basis with `axis` as its third vector.
    void Basis(const Vector3& axis, Vector3& outX, Vector3& outY)
    {
        const Vector3 helper = std::fabs(axis.y) < 0.99f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
        outX = Vector3Normalize(Vector3Cross(helper, axis));
        outY = Vector3Cross(axis, outX);
    }

    Vector3 Read3(float* const* s, int stream, uint32_t i)
    {
        return { s[stream][i], s[stream + 1][i], s[stream + 2][i] };
    }

    void Write3(float* const* s, int stream, uint32_t i, const Vector3& v)
    {
        s[stream][i] = v.x;
        s[stream + 1][i] = v.y;
        s[stream + 2][i] = v.z;
    }

    const std::string& ParamText(const ParticleModuleData& module, const char* name)
    {
        static const std::string empty;
        const ParticleParam* p = module.Find(name);
        return p ? p->text : empty;
    }

    ParticleBlend ParseBlend(const std::string& text)
    {
        if (text == "Alpha")
            return ParticleBlend::Alpha;
        if (text == "Premultiplied")
            return ParticleBlend::Premultiplied;
        return ParticleBlend::Additive;
    }

    // Default per-stream values of a new particle.
    float StreamDefault(int stream)
    {
        switch (stream)
        {
        case ParticleAttr::Color:
        case ParticleAttr::Color + 1:
        case ParticleAttr::Color + 2:
        case ParticleAttr::Color + 3:
        case ParticleAttr::Lifetime:
        case ParticleAttr::Mass:
        case ParticleAttr::MeshScale:
        case ParticleAttr::MeshScale + 1:
        case ParticleAttr::MeshScale + 2:
            return 1.0f;
        case ParticleAttr::SpriteSize:
        case ParticleAttr::SpriteSize + 1:
            return 0.1f;
        default:
            return 0.0f;
        }
    }
}

const ParticleGraphProgram* ParticleEmitterState::FindGraph(int id) const
{
    for (const auto& g : graphs)
    {
        if (g.first == id)
            return &g.second;
    }
    return nullptr;
}

ParticleSpriteSettings ReadParticleSpriteSettings(const ParticleRendererData& renderer)
{
    ParticleSpriteSettings s;
    s.material = ParamText(renderer, "Material");
    s.blend = ParseBlend(ParamText(renderer, "Blend"));
    const std::string& alignment = ParamText(renderer, "Alignment");
    s.alignment = alignment == "Velocity" ? 1 : (alignment == "Axis" ? 2 : 0);
    const Vector2 grid = textparse::ParseVector2(ParamText(renderer, "SubImages"), { 1.0f, 1.0f });
    s.columns = std::max(1, static_cast<int>(grid.x));
    s.rows = std::max(1, static_cast<int>(grid.y));
    s.sortBackToFront = ParamText(renderer, "Sort") == "BackToFront";
    s.lit = textparse::ParseBool(ParamText(renderer, "Lit"), false);
    s.sortOrder = std::atoi(ParamText(renderer, "SortOrder").c_str());
    return s;
}

bool ParticleBatchDrawsBefore(int sortOrderA, ParticleBlend blendA, float distanceSqA, int sortOrderB, ParticleBlend blendB, float distanceSqB)
{
    if (sortOrderA != sortOrderB)
        return sortOrderA < sortOrderB;
    const bool additiveA = blendA == ParticleBlend::Additive;
    const bool additiveB = blendB == ParticleBlend::Additive;
    if (additiveA != additiveB)
        return !additiveA;
    return distanceSqA > distanceSqB;
}

ParticleRibbonSettings ReadParticleRibbonSettings(const ParticleRendererData& renderer)
{
    ParticleRibbonSettings s;
    s.material = ParamText(renderer, "Material");
    s.blend = ParseBlend(ParamText(renderer, "Blend"));
    s.tile = ParamText(renderer, "TextureMode") == "Tile";
    if (const ParticleParam* p = renderer.Find("TileLength"))
        s.tileLength = std::max(0.001f, p->value.a[0]);
    s.lit = textparse::ParseBool(ParamText(renderer, "Lit"), false);
    s.sortOrder = std::atoi(ParamText(renderer, "SortOrder").c_str());
    return s;
}

void BuildParticleSpriteGeometry(const ParticleSpriteSettings& settings, std::vector<ParticleSpriteInstance>& sprites,
    const ParticleCameraBasis& camera, std::vector<ParticleVertex>& vertices, std::vector<uint32_t>& indices)
{
    if (settings.sortBackToFront)
    {
        const Vector3 eye = camera.position;
        const Vector3 fwd = camera.forward;
        std::sort(sprites.begin(), sprites.end(), [&](const ParticleSpriteInstance& a, const ParticleSpriteInstance& b)
            {
                const float da = (a.position[0] - eye.x) * fwd.x + (a.position[1] - eye.y) * fwd.y + (a.position[2] - eye.z) * fwd.z;
                const float db = (b.position[0] - eye.x) * fwd.x + (b.position[1] - eye.y) * fwd.y + (b.position[2] - eye.z) * fwd.z;
                return da > db;
            });
    }

    const int frames = settings.columns * settings.rows;
    const float du = 1.0f / static_cast<float>(settings.columns);
    const float dv = 1.0f / static_cast<float>(settings.rows);
    vertices.reserve(vertices.size() + sprites.size() * 4);
    indices.reserve(indices.size() + sprites.size() * 6);

    for (const ParticleSpriteInstance& s : sprites)
    {
        const Vector3 center(s.position[0], s.position[1], s.position[2]);
        Vector3 right = camera.right;
        Vector3 up = camera.up;
        Vector3 normal = camera.forward * -1.0f;
        float halfW = s.size[0] * 0.5f;
        float halfH = s.size[1] * 0.5f;
        bool rotate = true;

        if (settings.alignment == 1)
        {
            const Vector3 v(s.velocity[0], s.velocity[1], s.velocity[2]);
            const float speed = Vector3Length(v);
            if (speed > 1e-4f)
            {
                up = v / speed;
                const Vector3 toCamera = Vector3Normalize(camera.position - center);
                Vector3 side = Vector3Cross(up, toCamera);
                if (Vector3LengthSq(side) < 1e-8f)
                    side = camera.right;
                right = Vector3Normalize(side);
                normal = Vector3Cross(right, up);
                halfH += speed * s.stretch * 0.5f;
                rotate = false;
            }
        }
        else if (settings.alignment == 2)
        {
            Vector3 axis = Vector3Normalize(Vector3(s.axis[0], s.axis[1], s.axis[2]));
            if (Vector3LengthSq(axis) < 0.5f)
                axis = { 0.0f, 1.0f, 0.0f };
            normal = axis;
            Basis(axis, right, up);
        }

        if (rotate && s.rotation != 0.0f)
        {
            const float c = std::cos(s.rotation), sn = std::sin(s.rotation);
            const Vector3 r2 = right * c + up * sn;
            const Vector3 u2 = up * c - right * sn;
            right = r2;
            up = u2;
        }

        int frame = frames > 1 ? static_cast<int>(s.subImage) % frames : 0;
        if (frame < 0)
            frame += frames;
        const float u0 = static_cast<float>(frame % settings.columns) * du;
        const float v0 = static_cast<float>(frame / settings.columns) * dv;

        const Vector3 rx = right * halfW;
        const Vector3 uy = up * halfH;
        const Vector3 corners[4] = { center - rx + uy, center + rx + uy, center + rx - uy, center - rx - uy };
        const float uvs[4][2] = { { u0, v0 }, { u0 + du, v0 }, { u0 + du, v0 + dv }, { u0, v0 + dv } };
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (int k = 0; k < 4; ++k)
        {
            ParticleVertex vtx;
            vtx.position[0] = corners[k].x;
            vtx.position[1] = corners[k].y;
            vtx.position[2] = corners[k].z;
            vtx.uv[0] = uvs[k][0];
            vtx.uv[1] = uvs[k][1];
            std::memcpy(vtx.color, s.color, sizeof(vtx.color));
            vtx.normal[0] = normal.x;
            vtx.normal[1] = normal.y;
            vtx.normal[2] = normal.z;
            vertices.push_back(vtx);
        }
        const uint32_t quad[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
        indices.insert(indices.end(), quad, quad + 6);
    }
}

void BuildParticleRibbonGeometry(const ParticleRibbonSettings& settings, const std::vector<ParticleRibbonPoint>& points,
    const ParticleCameraBasis& camera, std::vector<ParticleVertex>& vertices, std::vector<uint32_t>& indices)
{
    if (points.size() < 2)
        return;
    const size_t n = points.size();
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    float distance = 0.0f;
    float total = 0.0f;
    for (size_t i = 1; i < n; ++i)
    {
        const Vector3 a(points[i - 1].position[0], points[i - 1].position[1], points[i - 1].position[2]);
        const Vector3 b(points[i].position[0], points[i].position[1], points[i].position[2]);
        total += Vector3Length(b - a);
    }
    Vector3 lastSide = camera.right;
    for (size_t i = 0; i < n; ++i)
    {
        const ParticleRibbonPoint& p = points[i];
        const Vector3 pos(p.position[0], p.position[1], p.position[2]);
        const ParticleRibbonPoint& prev = points[i > 0 ? i - 1 : 0];
        const ParticleRibbonPoint& next = points[i + 1 < n ? i + 1 : n - 1];
        const Vector3 tangent = Vector3(next.position[0] - prev.position[0], next.position[1] - prev.position[1], next.position[2] - prev.position[2]);
        if (i > 0)
        {
            const Vector3 a(points[i - 1].position[0], points[i - 1].position[1], points[i - 1].position[2]);
            distance += Vector3Length(pos - a);
        }
        const Vector3 toCamera = Vector3Normalize(camera.position - pos);
        Vector3 side = Vector3Cross(tangent, toCamera);
        if (Vector3LengthSq(side) > 1e-10f)
            lastSide = Vector3Normalize(side);
        side = lastSide;
        const Vector3 normal = Vector3Normalize(Vector3Cross(side, tangent));
        const float u = settings.tile ? distance / settings.tileLength : (total > 1e-6f ? distance / total : 0.0f);
        for (int k = 0; k < 2; ++k)
        {
            const Vector3 c = pos + side * (p.width * (k == 0 ? 0.5f : -0.5f));
            ParticleVertex vtx;
            vtx.position[0] = c.x;
            vtx.position[1] = c.y;
            vtx.position[2] = c.z;
            vtx.uv[0] = u;
            vtx.uv[1] = static_cast<float>(k);
            std::memcpy(vtx.color, p.color, sizeof(vtx.color));
            vtx.normal[0] = normal.x;
            vtx.normal[1] = normal.y;
            vtx.normal[2] = normal.z;
            vertices.push_back(vtx);
        }
    }
    for (uint32_t i = 0; i + 1 < n; ++i)
    {
        const uint32_t a = base + i * 2;
        const uint32_t seg[6] = { a, a + 1, a + 3, a, a + 3, a + 2 };
        indices.insert(indices.end(), seg, seg + 6);
    }
}

ParticleSystemInstance::Rotation3 ParticleSystemInstance::MakeRotation(const Vector3& r)
{
    // Rx * Ry * Rz as render::TransformToMatrix builds it.
    auto mul = [](const float* a, const float* b, float* out)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    };
    const float cx = std::cos(r.x), sx = std::sin(r.x);
    const float cy = std::cos(r.y), sy = std::sin(r.y);
    const float cz = std::cos(r.z), sz = std::sin(r.z);
    const float rx[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx };
    const float ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy };
    const float rz[9] = { cz, sz, 0, -sz, cz, 0, 0, 0, 1 };
    float xy[9];
    Rotation3 out;
    mul(rx, ry, xy);
    mul(xy, rz, out.m);
    return out;
}

Vector3 ParticleSystemInstance::Rotate(const Rotation3& r, const Vector3& v)
{
    return { v.x * r.m[0] + v.y * r.m[3] + v.z * r.m[6], v.x * r.m[1] + v.y * r.m[4] + v.z * r.m[7], v.x * r.m[2] + v.y * r.m[5] + v.z * r.m[8] };
}

Vector3 ParticleSystemInstance::RotateInverse(const Rotation3& r, const Vector3& v)
{
    return { v.x * r.m[0] + v.y * r.m[1] + v.z * r.m[2], v.x * r.m[3] + v.y * r.m[4] + v.z * r.m[5], v.x * r.m[6] + v.y * r.m[7] + v.z * r.m[8] };
}

Vector3 ParticleSystemInstance::ToWorldPoint(const ParticleEmitterData& d, const Vector3& p) const
{
    if (!d.localSpace)
        return p;
    return position + Rotate(rotationMatrix, { p.x * scale.x, p.y * scale.y, p.z * scale.z });
}

Vector3 ParticleSystemInstance::ToWorldVector(const ParticleEmitterData& d, const Vector3& v) const
{
    if (!d.localSpace)
        return v;
    return Rotate(rotationMatrix, { v.x * scale.x, v.y * scale.y, v.z * scale.z });
}

Vector3 ParticleSystemInstance::ToSimVector(const ParticleEmitterData& d, const Vector3& worldVector) const
{
    return d.localSpace ? RotateInverse(rotationMatrix, worldVector) : worldVector;
}

void ParticleSystemInstance::Init(const ParticleSystemData* systemData, uint32_t seed, std::vector<std::string>* outErrors)
{
    data = systemData;
    baseSeed = seed ? seed : ParticleHash(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this)) ^ 0xA511E9B3u);
    emitters.clear();
    userValues.clear();
    active = false;
    if (!data)
        return;
    for (const ParticleUserParam& p : data->userParams)
        userValues.push_back(p.value);

    emitters.resize(data->emitters.size());
    for (size_t i = 0; i < emitters.size(); ++i)
    {
        const ParticleEmitterData& d = data->emitters[i];
        ParticleEmitterState& e = emitters[i];
        e.streams.assign(static_cast<size_t>(ParticleAttr::kBuiltinStreams) + d.customAttributes.size() * 4, {});
        e.streamPointers.assign(e.streams.size(), nullptr);
        e.modules.resize(d.modules.size());
        for (size_t m = 0; m < d.modules.size(); ++m)
        {
            ParticleEmitterState::ModuleState& ms = e.modules[m];
            ms.salt = ParticleHash(static_cast<uint32_t>(i) * 7919u + static_cast<uint32_t>(m) * 104729u + 17u);
            if (d.modules[m].type == "SetAttribute")
            {
                const std::string& name = ParamText(d.modules[m], "Attribute");
                if (!FindParticleAttribute(d, name, ms.stream, ms.streamDim) && outErrors)
                    outErrors->push_back(d.name + ": Set Attribute: unknown attribute \"" + name + "\"");
            }
        }
        for (const ParticleGraphData& g : d.graphs)
        {
            e.graphs.emplace_back(g.id, ParticleGraphProgram());
            std::string error;
            if (!e.graphs.back().second.Compile(g, d, *data, &error) && outErrors)
                outErrors->push_back(d.name + ": graph " + std::to_string(g.id) + ": " + error);
        }
        ResetEmitter(i);
    }
}

void ParticleSystemInstance::ResetEmitter(size_t index)
{
    const ParticleEmitterData& d = data->emitters[index];
    ParticleEmitterState& e = emitters[index];
    e.count = 0;
    e.time = 0.0f;
    e.loopTime = 0.0f;
    e.loopIndex = 0;
    e.finishedSpawning = false;
    e.spawnRemainder = 0.0f;
    e.distanceRemainder = 0.0f;
    e.nextId = 0;
    e.seed = d.seed ? d.seed : ParticleHash(baseSeed + static_cast<uint32_t>(index) * 31u + frameCounter * 977u);
    for (ParticleEmitterState::ModuleState& ms : e.modules)
        ms.burstsFired = 0;
    e.boundsMin = e.boundsMax = position;
}

void ParticleSystemInstance::SetTransform(const Vector3& newPosition, const Vector3& newRotation, const Vector3& newScale, bool teleport)
{
    if (!transformSet || teleport)
        previousPosition = newPosition;
    position = newPosition;
    rotation = newRotation;
    scale = newScale;
    rotationMatrix = MakeRotation(newRotation);
    transformSet = true;
}

void ParticleSystemInstance::Activate(bool reset)
{
    if (!data)
        return;
    if (reset)
    {
        for (size_t i = 0; i < emitters.size(); ++i)
            ResetEmitter(i);
    }
    else
    {
        for (ParticleEmitterState& e : emitters)
            e.finishedSpawning = false;
    }
    active = true;
    previousPosition = position;
    if (reset && data->warmup > 0.0f)
    {
        const float step = 1.0f / 30.0f;
        for (float t = 0.0f; t < data->warmup; t += step)
            Tick(step, nullptr);
    }
}

void ParticleSystemInstance::Deactivate()
{
    active = false;
}

void ParticleSystemInstance::Clear()
{
    for (ParticleEmitterState& e : emitters)
        e.count = 0;
}

bool ParticleSystemInstance::IsComplete() const
{
    for (size_t i = 0; i < emitters.size(); ++i)
    {
        const ParticleEmitterState& e = emitters[i];
        if (e.count > 0)
            return false;
        if (active && data->emitters[i].enabled && !e.finishedSpawning)
            return false;
    }
    return true;
}

bool ParticleSystemInstance::SetUserFloat(const std::string& name, float value)
{
    if (!data)
        return false;
    for (size_t i = 0; i < data->userParams.size(); ++i)
    {
        if (data->userParams[i].name == name)
        {
            userValues[i] = value;
            return true;
        }
    }
    return false;
}

float ParticleSystemInstance::GetUserFloat(const std::string& name) const
{
    if (!data)
        return 0.0f;
    for (size_t i = 0; i < data->userParams.size(); ++i)
    {
        if (data->userParams[i].name == name)
            return userValues[i];
    }
    return 0.0f;
}

uint32_t ParticleSystemInstance::GetParticleCount() const
{
    uint32_t total = 0;
    for (const ParticleEmitterState& e : emitters)
        total += e.count;
    return total;
}

bool ParticleSystemInstance::GetBounds(Vector3& outMin, Vector3& outMax) const
{
    bool any = false;
    for (const ParticleEmitterState& e : emitters)
    {
        if (e.count == 0)
            continue;
        if (!any)
        {
            outMin = e.boundsMin;
            outMax = e.boundsMax;
            any = true;
            continue;
        }
        outMin = { std::min(outMin.x, e.boundsMin.x), std::min(outMin.y, e.boundsMin.y), std::min(outMin.z, e.boundsMin.z) };
        outMax = { std::max(outMax.x, e.boundsMax.x), std::max(outMax.y, e.boundsMax.y), std::max(outMax.z, e.boundsMax.z) };
    }
    return any;
}

ParticleEvalContext ParticleSystemInstance::MakeContext(size_t emitter, bool perParticle) const
{
    const ParticleEmitterState& e = emitters[emitter];
    const ParticleEmitterData& d = data->emitters[emitter];
    ParticleEvalContext ctx;
    ctx.streams = perParticle ? e.streamPointers.data() : nullptr;
    ctx.emitterTime = e.time;
    ctx.loopTime = d.duration > 0.0f ? std::min(e.loopTime / d.duration, 1.0f) : 0.0f;
    ctx.deltaTime = lastDelta;
    ctx.emitterPosition[0] = position.x;
    ctx.emitterPosition[1] = position.y;
    ctx.emitterPosition[2] = position.z;
    ctx.system = data;
    ctx.userValues = userValues.data();
    ctx.frameSeed = ParticleHash(frameCounter * 2654435761u + e.seed);
    return ctx;
}

void ParticleSystemInstance::Evaluate(size_t emitter, const ParticleValue& value, uint32_t salt, uint32_t begin, uint32_t count,
    bool perParticle, float* out) const
{
    const ParticleEmitterState& e = emitters[emitter];
    float* const* s = e.streamPointers.data();
    switch (value.mode)
    {
    case ParticleValueMode::Constant:
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(out + i * 4, value.a, sizeof(value.a));
        return;
    case ParticleValueMode::Random:
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t id = perParticle ? static_cast<uint32_t>(s[ParticleAttr::Id][begin + i]) : ParticleHash(frameCounter + e.seed);
            for (int c = 0; c < 4; ++c)
            {
                const float r = Rand(id ^ e.seed, salt, value.uniform ? 0u : static_cast<uint32_t>(c));
                out[i * 4 + c] = value.a[c] + (value.b[c] - value.a[c]) * r;
            }
        }
        return;
    case ParticleValueMode::Curve:
    case ParticleValueMode::Gradient:
    {
        const ParticleEmitterData& d = data->emitters[emitter];
        const float loopNorm = d.duration > 0.0f ? std::min(e.loopTime / d.duration, 1.0f) : 0.0f;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t p = begin + i;
            float x = 0.0f;
            switch (value.input)
            {
            case ParticleCurveInput::NormalizedAge: x = perParticle ? s[ParticleAttr::NormalizedAge][p] : loopNorm; break;
            case ParticleCurveInput::Age: x = perParticle ? s[ParticleAttr::Age][p] : e.time; break;
            case ParticleCurveInput::Speed: x = perParticle ? Vector3Length(Read3(s, ParticleAttr::Velocity, p)) : 0.0f; break;
            case ParticleCurveInput::Random: x = perParticle ? s[ParticleAttr::Random][p] : ParticleHash01(frameCounter ^ salt); break;
            case ParticleCurveInput::EmitterTime: x = e.time; break;
            case ParticleCurveInput::NormalizedLoopTime: x = loopNorm; break;
            }
            float* o = out + i * 4;
            if (value.mode == ParticleValueMode::Gradient)
                EvaluateParticleGradient(value.gradient, x, o);
            else
            {
                const float y = EvaluateParticleCurve(value.curve, x);
                for (int c = 0; c < 4; ++c)
                    o[c] = value.a[c] * y;
            }
        }
        return;
    }
    case ParticleValueMode::Graph:
        if (const ParticleGraphProgram* program = e.FindGraph(value.graph))
        {
            program->Run(MakeContext(emitter, perParticle), begin, count, out);
            return;
        }
        std::fill(out, out + static_cast<size_t>(count) * 4, 0.0f);
        return;
    }
}

const float* ParticleSystemInstance::EvaluateParam(size_t emitter, const ParticleModuleData& module, uint32_t salt, const char* name,
    uint32_t begin, uint32_t count, bool perParticle, int slot) const
{
    if (scratch.size() < kScratchSlots)
        scratch.resize(kScratchSlots);
    std::vector<float>& buffer = scratch[static_cast<size_t>(slot)];
    buffer.resize(static_cast<size_t>(std::max(count, 1u)) * 4);
    const ParticleParam* param = module.Find(name);
    if (!param)
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        return buffer.data();
    }
    uint32_t paramSalt = salt;
    for (const char* c = name; *c; ++c)
        paramSalt = paramSalt * 31u + static_cast<uint32_t>(*c);
    Evaluate(emitter, param->value, paramSalt, begin, count, perParticle, buffer.data());
    return buffer.data();
}

void ParticleSystemInstance::Grow(ParticleEmitterState& e, uint32_t capacity)
{
    if (!e.streams.empty() && e.streams[0].size() >= capacity)
        return;
    const size_t newSize = std::max<size_t>(capacity, e.streams.empty() ? 0 : e.streams[0].size() * 2);
    for (size_t s = 0; s < e.streams.size(); ++s)
    {
        e.streams[s].resize(newSize);
        e.streamPointers[s] = e.streams[s].data();
    }
    e.kill.resize(newSize);
}

void ParticleSystemInstance::Compact(ParticleEmitterState& e)
{
    uint32_t i = 0;
    while (i < e.count)
    {
        if (!e.kill[i])
        {
            ++i;
            continue;
        }
        const uint32_t last = e.count - 1;
        if (i != last)
        {
            for (std::vector<float>& stream : e.streams)
                stream[i] = stream[last];
            e.kill[i] = e.kill[last];
        }
        --e.count;
    }
}

void ParticleSystemInstance::UpdateBounds(ParticleEmitterState& e, const ParticleEmitterData& d)
{
    if (e.count == 0)
    {
        e.boundsMin = e.boundsMax = position;
        return;
    }
    float* const* s = e.streamPointers.data();
    Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    float maxSize = 0.0f;
    for (uint32_t i = 0; i < e.count; ++i)
    {
        const Vector3 p = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
        mn = { std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z) };
        mx = { std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z) };
        maxSize = std::max({ maxSize, s[ParticleAttr::SpriteSize][i], s[ParticleAttr::SpriteSize + 1][i] });
    }
    const Vector3 pad(maxSize, maxSize, maxSize);
    e.boundsMin = mn - pad;
    e.boundsMax = mx + pad;
}

void ParticleSystemInstance::Tick(float dt, const IParticleCollision* collision)
{
    if (!data)
        return;
    dt *= timeScale;
    if (dt <= 0.0f)
        return;
    dt = std::min(dt, 0.1f);
    lastDelta = dt;
    ++frameCounter;
    velocity = (position - previousPosition) / dt;
    for (size_t i = 0; i < emitters.size(); ++i)
    {
        if (data->emitters[i].enabled)
            TickEmitter(i, dt, collision);
        else
            emitters[i].count = 0;
    }
    previousPosition = position;
}

void ParticleSystemInstance::TickEmitter(size_t index, float dt, const IParticleCollision* collision)
{
    const ParticleEmitterData& d = data->emitters[index];
    ParticleEmitterState& e = emitters[index];

    UpdateParticles(index, dt, collision);

    e.time += dt;
    const float activeTime = e.time - d.delay;
    uint32_t rateCount = 0, burstCount = 0;
    if (active && !e.finishedSpawning && activeTime >= 0.0f)
    {
        e.loopTime = activeTime - static_cast<float>(e.loopIndex) * d.duration;
        if (d.duration > 0.0f)
        {
            while (e.loopTime >= d.duration)
            {
                if (d.loop && (d.loopCount == 0 || static_cast<uint32_t>(e.loopIndex) + 1 < d.loopCount))
                {
                    ++e.loopIndex;
                    e.loopTime -= d.duration;
                    for (ParticleEmitterState::ModuleState& ms : e.modules)
                        ms.burstsFired = 0;
                }
                else
                {
                    e.finishedSpawning = true;
                    break;
                }
            }
        }
        else
            e.loopTime = activeTime;

        if (!e.finishedSpawning)
            rateCount = EvaluateSpawnCount(index, std::min(dt, activeTime), burstCount);
    }
    else if (!active)
        e.spawnRemainder = 0.0f;

    if (rateCount + burstCount > 0)
        SpawnParticles(index, rateCount, burstCount, dt);
    UpdateBounds(e, d);
}

uint32_t ParticleSystemInstance::EvaluateSpawnCount(size_t index, float dt, uint32_t& outBurst)
{
    const ParticleEmitterData& d = data->emitters[index];
    ParticleEmitterState& e = emitters[index];
    float rate = 0.0f;
    outBurst = 0;
    for (size_t m = 0; m < d.modules.size(); ++m)
    {
        const ParticleModuleData& module = d.modules[m];
        if (!module.enabled || module.stage != ParticleStage::EmitterUpdate)
            continue;
        ParticleEmitterState::ModuleState& ms = e.modules[m];
        if (module.type == "SpawnRate")
            rate += std::max(0.0f, EvaluateParam(index, module, ms.salt, "Rate", 0, 1, false, 0)[0]) * dt;
        else if (module.type == "SpawnPerUnit")
        {
            const float perUnit = std::max(0.0f, EvaluateParam(index, module, ms.salt, "PerUnit", 0, 1, false, 0)[0]);
            const int maxPerFrame = std::max(0, std::atoi(ParamText(module, "MaxPerFrame").c_str()));
            float n = Vector3Length(position - previousPosition) * perUnit;
            n = std::min(n, static_cast<float>(maxPerFrame > 0 ? maxPerFrame : 100000));
            rate += n;
        }
        else if (module.type == "SpawnBurst")
        {
            const float time = EvaluateParam(index, module, ms.salt, "Time", 0, 1, false, 0)[0];
            const float interval = EvaluateParam(index, module, ms.salt, "Interval", 0, 1, false, 1)[0];
            const int repeat = interval > 0.0f ? std::max(1, std::atoi(ParamText(module, "Repeat").c_str())) : 1;
            while (ms.burstsFired < repeat && e.loopTime >= time + interval * static_cast<float>(ms.burstsFired))
            {
                const float count = EvaluateParam(index, module, ms.salt + static_cast<uint32_t>(ms.burstsFired), "Count", 0, 1, false, 2)[0];
                outBurst += static_cast<uint32_t>(std::max(0.0f, std::round(count)));
                ++ms.burstsFired;
            }
        }
    }
    e.spawnRemainder += rate;
    const float whole = std::floor(e.spawnRemainder);
    e.spawnRemainder -= whole;
    return static_cast<uint32_t>(whole);
}

void ParticleSystemInstance::SpawnParticles(size_t index, uint32_t rateCount, uint32_t burstCount, float dt)
{
    const ParticleEmitterData& d = data->emitters[index];
    ParticleEmitterState& e = emitters[index];
    const uint32_t room = d.maxParticles > e.count ? d.maxParticles - e.count : 0u;
    burstCount = std::min(burstCount, room);
    rateCount = std::min(rateCount, room - burstCount);
    const uint32_t total = rateCount + burstCount;
    if (total == 0)
        return;

    const uint32_t begin = e.count;
    Grow(e, begin + total);
    e.count += total;
    float* const* s = e.streamPointers.data();
    for (uint32_t i = begin; i < e.count; ++i)
    {
        for (size_t stream = 0; stream < e.streams.size(); ++stream)
            s[stream][i] = StreamDefault(static_cast<int>(stream));
        const uint32_t id = e.nextId++;
        s[ParticleAttr::Id][i] = static_cast<float>(id);
        s[ParticleAttr::Random][i] = Rand(id, e.seed, 991u);
        // Rate-spawned particles are spread over the frame; bursts start together.
        const uint32_t k = i - begin;
        s[ParticleAttr::Age][i] = k < rateCount ? dt * (1.0f - (static_cast<float>(k) + 0.5f) / static_cast<float>(rateCount)) : 0.0f;
        e.kill[i] = 0;
    }

    for (size_t m = 0; m < d.modules.size(); ++m)
    {
        if (d.modules[m].enabled && d.modules[m].stage == ParticleStage::ParticleSpawn)
            RunModule(index, m, begin, total, dt, nullptr);
    }

    for (uint32_t i = begin; i < e.count; ++i)
    {
        const float age = s[ParticleAttr::Age][i];
        Vector3 p = Read3(s, ParticleAttr::Position, i);
        Vector3 v = Read3(s, ParticleAttr::Velocity, i);
        if (!d.localSpace)
        {
            const float t = dt > 0.0f ? 1.0f - age / dt : 1.0f;
            const Vector3 origin = previousPosition + (position - previousPosition) * t;
            p = origin + Rotate(rotationMatrix, { p.x * scale.x, p.y * scale.y, p.z * scale.z });
            v = Rotate(rotationMatrix, v);
        }
        p += v * age;
        Write3(s, ParticleAttr::Position, i, p);
        Write3(s, ParticleAttr::Velocity, i, v);
        s[ParticleAttr::SpriteRotation][i] += s[ParticleAttr::RotationRate][i] * age;
        const float lifetime = std::max(s[ParticleAttr::Lifetime][i], 0.001f);
        s[ParticleAttr::Lifetime][i] = lifetime;
        s[ParticleAttr::NormalizedAge][i] = std::min(age / lifetime, 1.0f);
        for (int c = 0; c < 4; ++c)
            s[ParticleAttr::InitialColor + c][i] = s[ParticleAttr::Color + c][i];
        for (int c = 0; c < 2; ++c)
            s[ParticleAttr::InitialSize + c][i] = s[ParticleAttr::SpriteSize + c][i];
        for (int c = 0; c < 3; ++c)
            s[ParticleAttr::InitialMeshScale + c][i] = s[ParticleAttr::MeshScale + c][i];
    }
}

void ParticleSystemInstance::UpdateParticles(size_t index, float dt, const IParticleCollision* collision)
{
    const ParticleEmitterData& d = data->emitters[index];
    ParticleEmitterState& e = emitters[index];
    if (e.count == 0)
        return;
    float* const* s = e.streamPointers.data();

    for (uint32_t i = 0; i < e.count; ++i)
    {
        float& age = s[ParticleAttr::Age][i];
        age += dt;
        const float lifetime = s[ParticleAttr::Lifetime][i];
        e.kill[i] = age >= lifetime ? 1 : 0;
        s[ParticleAttr::NormalizedAge][i] = std::min(age / lifetime, 1.0f);
    }
    Compact(e);
    if (e.count == 0)
        return;

    auto isPostIntegrate = [](const std::string& type) { return type == "Collision" || type == "KillVolume"; };
    for (size_t m = 0; m < d.modules.size(); ++m)
    {
        const ParticleModuleData& module = d.modules[m];
        if (module.enabled && module.stage == ParticleStage::ParticleUpdate && !isPostIntegrate(module.type))
            RunModule(index, m, 0, e.count, dt, collision);
    }

    for (uint32_t i = 0; i < e.count; ++i)
    {
        Write3(s, ParticleAttr::Position, i, Read3(s, ParticleAttr::Position, i) + Read3(s, ParticleAttr::Velocity, i) * dt);
        s[ParticleAttr::SpriteRotation][i] += s[ParticleAttr::RotationRate][i] * dt;
        Write3(s, ParticleAttr::MeshRotation, i, Read3(s, ParticleAttr::MeshRotation, i) + Read3(s, ParticleAttr::MeshAngularVelocity, i) * dt);
    }

    for (size_t m = 0; m < d.modules.size(); ++m)
    {
        const ParticleModuleData& module = d.modules[m];
        if (module.enabled && module.stage == ParticleStage::ParticleUpdate && isPostIntegrate(module.type))
            RunModule(index, m, 0, e.count, dt, collision);
    }
    Compact(e);
}

void ParticleSystemInstance::RunModule(size_t emitterIndex, size_t moduleIndex, uint32_t begin, uint32_t count, float dt,
    const IParticleCollision* collision)
{
    const ParticleEmitterData& d = data->emitters[emitterIndex];
    ParticleEmitterState& e = emitters[emitterIndex];
    const ParticleModuleData& module = d.modules[moduleIndex];
    const ParticleEmitterState::ModuleState& ms = e.modules[moduleIndex];
    float* const* s = e.streamPointers.data();
    const std::string& type = module.type;
    const bool spawnStage = module.stage == ParticleStage::ParticleSpawn;
    const uint32_t end = begin + count;
    auto param = [&](const char* name, int slot) { return EvaluateParam(emitterIndex, module, ms.salt, name, begin, count, true, slot); };
    auto vec = [](const float* values, uint32_t k) { return Vector3(values[k * 4], values[k * 4 + 1], values[k * 4 + 2]); };
    auto id = [&](uint32_t i) { return static_cast<uint32_t>(s[ParticleAttr::Id][i]) ^ e.seed; };
    // Simulation-space center of the system (local emitters simulate around 0).
    const Vector3 center = d.localSpace ? Vector3() : position;

    if (type == "InitializeParticle")
    {
        const float* lifetime = param("Lifetime", 0);
        const float* color = param("Color", 1);
        const float* size = param("SpriteSize", 2);
        const float* rot = param("SpriteRotation", 3);
        const float* mass = param("Mass", 4);
        const float* meshScale = param("MeshScale", 5);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            s[ParticleAttr::Lifetime][i] = std::max(lifetime[k * 4], 0.001f);
            for (int c = 0; c < 4; ++c)
                s[ParticleAttr::Color + c][i] = color[k * 4 + c];
            s[ParticleAttr::SpriteSize][i] = size[k * 4];
            s[ParticleAttr::SpriteSize + 1][i] = size[k * 4 + 1];
            s[ParticleAttr::SpriteRotation][i] = rot[k * 4] * kDegToRad;
            s[ParticleAttr::Mass][i] = std::max(mass[k * 4], 0.0001f);
            Write3(s, ParticleAttr::MeshScale, i, vec(meshScale, k));
        }
    }
    else if (type == "ShapeLocation")
    {
        const std::string& shape = ParamText(module, "Shape");
        const bool surface = textparse::ParseBool(ParamText(module, "SurfaceOnly"), false);
        const float* radius = param("Radius", 0);
        const float* box = param("BoxSize", 1);
        const float* height = param("Height", 2);
        const float* angle = param("ConeAngle", 3);
        const float* offset = param("Offset", 4);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            const uint32_t pid = id(i);
            const float r = radius[k * 4];
            Vector3 p;
            if (shape == "Sphere")
            {
                const Vector3 dir = RandomUnitVector(pid, ms.salt, 0);
                p = dir * (surface ? r : r * std::cbrt(Rand(pid, ms.salt, 2)));
            }
            else if (shape == "Box")
            {
                const Vector3 half = vec(box, k) * 0.5f;
                p = { (Rand(pid, ms.salt, 0) * 2.0f - 1.0f) * half.x, (Rand(pid, ms.salt, 1) * 2.0f - 1.0f) * half.y,
                    (Rand(pid, ms.salt, 2) * 2.0f - 1.0f) * half.z };
                if (surface)
                {
                    const int axis = static_cast<int>(Rand(pid, ms.salt, 3) * 3.0f) % 3;
                    const float sign = Rand(pid, ms.salt, 4) < 0.5f ? -1.0f : 1.0f;
                    if (axis == 0) p.x = half.x * sign;
                    else if (axis == 1) p.y = half.y * sign;
                    else p.z = half.z * sign;
                }
            }
            else if (shape == "Cylinder" || shape == "Ring")
            {
                const float a = Rand(pid, ms.salt, 0) * kTwoPi;
                const float rr = (shape == "Ring" || surface) ? r : r * std::sqrt(Rand(pid, ms.salt, 1));
                const float y = shape == "Ring" ? 0.0f : Rand(pid, ms.salt, 2) * height[k * 4];
                p = { std::cos(a) * rr, y, std::sin(a) * rr };
            }
            else if (shape == "Cone")
            {
                const float h = Rand(pid, ms.salt, 2) * height[k * 4];
                const float maxR = h * std::tan(std::clamp(angle[k * 4], 0.0f, 89.0f) * kDegToRad);
                const float a = Rand(pid, ms.salt, 0) * kTwoPi;
                const float rr = surface ? maxR : maxR * std::sqrt(Rand(pid, ms.salt, 1));
                p = { std::cos(a) * rr, h, std::sin(a) * rr };
            }
            Write3(s, ParticleAttr::Position, i, Read3(s, ParticleAttr::Position, i) + p + vec(offset, k));
        }
    }
    else if (type == "AddVelocity")
    {
        const std::string& mode = ParamText(module, "Mode");
        const float* velocityParam = param("Velocity", 0);
        const float* speed = param("Speed", 1);
        const float* axisParam = param("ConeAxis", 2);
        const float* angle = param("ConeAngle", 3);
        const float scaleBy = spawnStage ? 1.0f : dt;
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            Vector3 add;
            if (mode == "FromCenter")
            {
                // Spawn positions are still relative to the system here; in Update they are in simulation space.
                Vector3 rel = Read3(s, ParticleAttr::Position, i) - (spawnStage ? Vector3() : center);
                Vector3 dir = Vector3Normalize(rel);
                if (Vector3LengthSq(dir) < 0.5f)
                    dir = RandomUnitVector(id(i), ms.salt, 5);
                add = dir * speed[k * 4];
            }
            else if (mode == "Cone")
            {
                Vector3 axis = Vector3Normalize(vec(axisParam, k));
                if (Vector3LengthSq(axis) < 0.5f)
                    axis = { 0.0f, 1.0f, 0.0f };
                Vector3 bx, by;
                Basis(axis, bx, by);
                const float cosMax = std::cos(std::clamp(angle[k * 4], 0.0f, 180.0f) * kDegToRad);
                const float cosT = 1.0f - Rand(id(i), ms.salt, 6) * (1.0f - cosMax);
                const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
                const float phi = Rand(id(i), ms.salt, 7) * kTwoPi;
                add = (axis * cosT + bx * (sinT * std::cos(phi)) + by * (sinT * std::sin(phi))) * speed[k * 4];
            }
            else
                add = vec(velocityParam, k);
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + add * scaleBy);
        }
    }
    else if (type == "InheritVelocity")
    {
        if (d.localSpace)
            return;
        const float* scaleParam = param("Scale", 0);
        // Spawn velocities are rotated into world space afterwards; pre-rotate the inherited part back.
        const Vector3 local = RotateInverse(rotationMatrix, velocity);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + local * scaleParam[k * 4]);
    }
    else if (type == "InitializeRotation")
    {
        const float* rate = param("SpriteRotationRate", 0);
        const float* meshRot = param("MeshRotation", 1);
        const float* angular = param("MeshAngularVelocity", 2);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            s[ParticleAttr::RotationRate][i] = rate[k * 4] * kDegToRad;
            Write3(s, ParticleAttr::MeshRotation, i, vec(meshRot, k) * kDegToRad);
            Write3(s, ParticleAttr::MeshAngularVelocity, i, vec(angular, k) * kDegToRad);
        }
    }
    else if (type == "SetAttribute")
    {
        if (ms.stream < 0)
            return;
        const float* value = param("Value", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            for (int c = 0; c < ms.streamDim; ++c)
                s[ms.stream + c][i] = value[k * 4 + c];
        }
    }
    else if (type == "Gravity")
    {
        const float* acc = param("Acceleration", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + ToSimVector(d, vec(acc, k)) * dt);
    }
    else if (type == "Drag")
    {
        const float* drag = param("Drag", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) * std::exp(-std::max(drag[k * 4], 0.0f) * dt));
    }
    else if (type == "Force")
    {
        const bool localForce = ParamText(module, "Space") == "Local";
        const float* force = param("Force", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            Vector3 f = vec(force, k);
            if (localForce && !d.localSpace)
                f = Rotate(rotationMatrix, f);
            else if (!localForce)
                f = ToSimVector(d, f);
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + f * (dt / s[ParticleAttr::Mass][i]));
        }
    }
    else if (type == "CurlNoise")
    {
        const float* strength = param("Strength", 0);
        const float* frequency = param("Frequency", 1);
        const float* pan = param("Pan", 2);
        const float eps = 0.05f;
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            const Vector3 p = Read3(s, ParticleAttr::Position, i) * frequency[k * 4] + vec(pan, k) * e.time;
            float px0[3], px1[3], py0[3], py1[3], pz0[3], pz1[3];
            ParticleVectorNoise(p.x - eps, p.y, p.z, px0);
            ParticleVectorNoise(p.x + eps, p.y, p.z, px1);
            ParticleVectorNoise(p.x, p.y - eps, p.z, py0);
            ParticleVectorNoise(p.x, p.y + eps, p.z, py1);
            ParticleVectorNoise(p.x, p.y, p.z - eps, pz0);
            ParticleVectorNoise(p.x, p.y, p.z + eps, pz1);
            const float inv = 1.0f / (2.0f * eps);
            const Vector3 curl((py1[2] - py0[2] - (pz1[1] - pz0[1])) * inv, (pz1[0] - pz0[0] - (px1[2] - px0[2])) * inv,
                (px1[1] - px0[1] - (py1[0] - py0[0])) * inv);
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + curl * (strength[k * 4] * dt * 0.5f));
        }
    }
    else if (type == "Vortex")
    {
        const float* axisParam = param("Axis", 0);
        const float* strength = param("Strength", 1);
        const float* pull = param("Pull", 2);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            Vector3 axis = Vector3Normalize(d.localSpace ? vec(axisParam, k) : Rotate(rotationMatrix, vec(axisParam, k)));
            if (Vector3LengthSq(axis) < 0.5f)
                continue;
            const Vector3 r = Read3(s, ParticleAttr::Position, i) - center;
            const Vector3 radial = r - axis * Vector3Dot(r, axis);
            const Vector3 radialDir = Vector3Normalize(radial);
            const Vector3 tangent = Vector3Cross(axis, radialDir);
            const Vector3 dv = tangent * strength[k * 4] - radialDir * pull[k * 4];
            Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + dv * dt);
        }
    }
    else if (type == "PointAttraction")
    {
        const float* target = param("Position", 0);
        const float* strength = param("Strength", 1);
        const float* radius = param("Radius", 2);
        const float* killRadius = param("KillRadius", 3);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            const Vector3 t = center + (d.localSpace ? vec(target, k) : Rotate(rotationMatrix, vec(target, k)));
            const Vector3 delta = t - Read3(s, ParticleAttr::Position, i);
            const float dist = Vector3Length(delta);
            if (dist < killRadius[k * 4])
            {
                e.kill[i] = 1;
                continue;
            }
            const float falloff = radius[k * 4] > 0.0f ? std::clamp(1.0f - dist / radius[k * 4], 0.0f, 1.0f) : 1.0f;
            if (dist > 1e-5f)
                Write3(s, ParticleAttr::Velocity, i, Read3(s, ParticleAttr::Velocity, i) + delta * (strength[k * 4] * falloff * dt / dist));
        }
    }
    else if (type == "LimitSpeed")
    {
        const float* maxSpeed = param("MaxSpeed", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            const Vector3 v = Read3(s, ParticleAttr::Velocity, i);
            const float speed = Vector3Length(v);
            if (speed > maxSpeed[k * 4] && speed > 0.0f)
                Write3(s, ParticleAttr::Velocity, i, v * (std::max(maxSpeed[k * 4], 0.0f) / speed));
        }
    }
    else if (type == "ScaleColor")
    {
        const float* scaleParam = param("Scale", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
            for (int c = 0; c < 4; ++c)
                s[ParticleAttr::Color + c][i] = s[ParticleAttr::InitialColor + c][i] * scaleParam[k * 4 + c];
    }
    else if (type == "ScaleSize")
    {
        const float* scaleParam = param("Scale", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
            for (int c = 0; c < 2; ++c)
                s[ParticleAttr::SpriteSize + c][i] = s[ParticleAttr::InitialSize + c][i] * scaleParam[k * 4 + c];
    }
    else if (type == "ScaleMeshSize")
    {
        const float* scaleParam = param("Scale", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
            for (int c = 0; c < 3; ++c)
                s[ParticleAttr::MeshScale + c][i] = s[ParticleAttr::InitialMeshScale + c][i] * scaleParam[k * 4 + c];
    }
    else if (type == "SubUVAnimation")
    {
        const std::string& mode = ParamText(module, "Mode");
        const float* frames = param("Frames", 0);
        const float* rate = param("FrameRate", 1);
        const float* start = param("StartFrame", 2);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            const float n = std::max(1.0f, std::floor(frames[k * 4]));
            float frame;
            if (mode == "FrameRate")
                frame = std::fmod(std::floor(s[ParticleAttr::Age][i] * rate[k * 4]), n);
            else if (mode == "Random")
                frame = std::floor(s[ParticleAttr::Random][i] * n);
            else
                frame = std::min(std::floor(s[ParticleAttr::NormalizedAge][i] * n), n - 1.0f);
            s[ParticleAttr::SubImage][i] = start[k * 4] + frame;
        }
    }
    else if (type == "KillIf")
    {
        const float* condition = param("Condition", 0);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            if (condition[k * 4] > 0.5f)
                e.kill[i] = 1;
        }
    }
    else if (type == "KillVolume")
    {
        const bool box = ParamText(module, "Shape") == "Box";
        const bool worldSpace = ParamText(module, "Space") == "World";
        const bool invert = textparse::ParseBool(ParamText(module, "Invert"), false);
        const float* c = param("Center", 0);
        const float* radius = param("Radius", 1);
        const float* size = param("Size", 2);
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            Vector3 p = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
            if (!worldSpace)
            {
                const Vector3 rel = RotateInverse(rotationMatrix, p - position);
                p = { rel.x / scale.x, rel.y / scale.y, rel.z / scale.z };
            }
            const Vector3 rel = p - vec(c, k);
            bool inside;
            if (box)
            {
                const Vector3 half = vec(size, k) * 0.5f;
                inside = std::fabs(rel.x) <= half.x && std::fabs(rel.y) <= half.y && std::fabs(rel.z) <= half.z;
            }
            else
                inside = Vector3LengthSq(rel) <= radius[k * 4] * radius[k * 4];
            if (inside != invert)
                e.kill[i] = 1;
        }
    }
    else if (type == "Collision")
    {
        const bool plane = ParamText(module, "Mode") == "GroundPlane";
        const std::string& response = ParamText(module, "Response");
        const float* height = param("PlaneHeight", 0);
        const float* radius = param("Radius", 1);
        const float* restitution = param("Restitution", 2);
        const float* friction = param("Friction", 3);
        if (!plane && !collision)
            return;
        for (uint32_t i = begin, k = 0; i < end; ++i, ++k)
        {
            const float r = std::max(radius[k * 4], 0.0f);
            Vector3 pos = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
            Vector3 vel = ToWorldVector(d, Read3(s, ParticleAttr::Velocity, i));
            Vector3 hitPoint, normal;
            bool hit = false;
            if (plane)
            {
                if (pos.y < height[k * 4] + r)
                {
                    hit = true;
                    normal = { 0.0f, 1.0f, 0.0f };
                    hitPoint = { pos.x, height[k * 4], pos.z };
                }
            }
            else
            {
                const Vector3 from = pos - vel * dt;
                const Vector3 travel = pos - from;
                const float len = Vector3Length(travel);
                if (len > 1e-5f)
                {
                    const Vector3 to = pos + travel * (r / len);
                    hit = collision->Raycast(from, to, hitPoint, normal);
                }
            }
            if (!hit)
                continue;
            if (response == "Kill")
            {
                e.kill[i] = 1;
                continue;
            }
            pos = hitPoint + normal * r;
            const float vn = Vector3Dot(vel, normal);
            if (vn < 0.0f)
            {
                Vector3 normalPart = normal * vn;
                Vector3 tangentPart = vel - normalPart;
                tangentPart = tangentPart * std::clamp(1.0f - friction[k * 4], 0.0f, 1.0f);
                normalPart = response == "Stop" || -vn * restitution[k * 4] < 0.3f ? Vector3() : normalPart * -restitution[k * 4];
                vel = response == "Stop" ? Vector3() : tangentPart + normalPart;
            }
            if (d.localSpace)
            {
                const Vector3 rel = RotateInverse(rotationMatrix, pos - position);
                pos = { rel.x / scale.x, rel.y / scale.y, rel.z / scale.z };
                const Vector3 lv = RotateInverse(rotationMatrix, vel);
                vel = { lv.x / scale.x, lv.y / scale.y, lv.z / scale.z };
            }
            Write3(s, ParticleAttr::Position, i, pos);
            Write3(s, ParticleAttr::Velocity, i, vel);
        }
    }
}

void ParticleSystemInstance::ExtractSprites(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleSpriteInstance>& out) const
{
    const ParticleEmitterState& e = emitters[emitter];
    const ParticleEmitterData& d = data->emitters[emitter];
    if (e.count == 0)
        return;
    float* const* s = e.streamPointers.data();
    const uint32_t salt = 0x5B1Eu;
    const float* emissive = EvaluateParam(emitter, renderer, salt, "Emissive", 0, e.count, true, 0);
    const float* axis = EvaluateParam(emitter, renderer, salt, "Axis", 0, e.count, true, 1);
    const float* stretch = EvaluateParam(emitter, renderer, salt, "VelocityStretch", 0, e.count, true, 2);
    const float sizeScale = (std::fabs(scale.x) + std::fabs(scale.y) + std::fabs(scale.z)) / 3.0f;
    const size_t first = out.size();
    out.resize(first + e.count);
    for (uint32_t i = 0; i < e.count; ++i)
    {
        ParticleSpriteInstance& o = out[first + i];
        const Vector3 p = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
        const Vector3 v = ToWorldVector(d, Read3(s, ParticleAttr::Velocity, i));
        const Vector3 ax = ToWorldVector(d, { axis[i * 4], axis[i * 4 + 1], axis[i * 4 + 2] });
        o.position[0] = p.x;
        o.position[1] = p.y;
        o.position[2] = p.z;
        o.velocity[0] = v.x;
        o.velocity[1] = v.y;
        o.velocity[2] = v.z;
        o.axis[0] = ax.x;
        o.axis[1] = ax.y;
        o.axis[2] = ax.z;
        const float sz = d.localSpace ? sizeScale : 1.0f;
        o.size[0] = s[ParticleAttr::SpriteSize][i] * sz;
        o.size[1] = s[ParticleAttr::SpriteSize + 1][i] * sz;
        o.rotation = s[ParticleAttr::SpriteRotation][i];
        for (int c = 0; c < 3; ++c)
            o.color[c] = s[ParticleAttr::Color + c][i] * emissive[i * 4];
        o.color[3] = std::clamp(s[ParticleAttr::Color + 3][i], 0.0f, 1.0f);
        o.subImage = s[ParticleAttr::SubImage][i];
        o.stretch = stretch[i * 4];
    }
}

void ParticleSystemInstance::ExtractRibbon(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleRibbonPoint>& out) const
{
    const ParticleEmitterState& e = emitters[emitter];
    const ParticleEmitterData& d = data->emitters[emitter];
    if (e.count < 2)
        return;
    float* const* s = e.streamPointers.data();
    const float* width = EvaluateParam(emitter, renderer, 0x41BBu, "Width", 0, e.count, true, 0);
    const float* emissive = EvaluateParam(emitter, renderer, 0x41BBu, "Emissive", 0, e.count, true, 1);
    std::vector<uint32_t> order(e.count);
    for (uint32_t i = 0; i < e.count; ++i)
        order[i] = i;
    // Newest particle first: the trail starts at the emitter.
    std::sort(order.begin(), order.end(), [s](uint32_t a, uint32_t b) { return s[ParticleAttr::Id][a] > s[ParticleAttr::Id][b]; });
    out.reserve(out.size() + e.count);
    for (uint32_t i : order)
    {
        ParticleRibbonPoint p;
        const Vector3 w = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
        p.position[0] = w.x;
        p.position[1] = w.y;
        p.position[2] = w.z;
        p.width = width[i * 4];
        for (int c = 0; c < 3; ++c)
            p.color[c] = s[ParticleAttr::Color + c][i] * emissive[i * 4];
        p.color[3] = std::clamp(s[ParticleAttr::Color + 3][i], 0.0f, 1.0f);
        out.push_back(p);
    }
}

void ParticleSystemInstance::ExtractMeshes(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleMeshInstance>& out) const
{
    const ParticleEmitterState& e = emitters[emitter];
    const ParticleEmitterData& d = data->emitters[emitter];
    float* const* s = e.streamPointers.data();
    const bool velocityAligned = ParamText(renderer, "Orientation") == "Velocity";
    for (uint32_t i = 0; i < e.count; ++i)
    {
        Vector3 axes[3];
        if (velocityAligned)
        {
            Vector3 fwd = Vector3Normalize(ToWorldVector(d, Read3(s, ParticleAttr::Velocity, i)));
            if (Vector3LengthSq(fwd) < 0.5f)
                fwd = { 0.0f, 0.0f, 1.0f };
            Basis(fwd, axes[0], axes[1]);
            axes[2] = fwd;
        }
        else
        {
            const Rotation3 local = MakeRotation(Read3(s, ParticleAttr::MeshRotation, i));
            for (int r = 0; r < 3; ++r)
            {
                const Vector3 row(local.m[r * 3], local.m[r * 3 + 1], local.m[r * 3 + 2]);
                axes[r] = d.localSpace ? Rotate(rotationMatrix, row) : row;
            }
        }
        const Vector3 ms = Read3(s, ParticleAttr::MeshScale, i);
        const float scales[3] = { ms.x, ms.y, ms.z };
        const Vector3 p = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
        ParticleMeshInstance inst;
        for (int r = 0; r < 3; ++r)
        {
            const float sc = scales[r] * (d.localSpace ? (r == 0 ? scale.x : (r == 1 ? scale.y : scale.z)) : 1.0f);
            inst.world[r * 4] = axes[r].x * sc;
            inst.world[r * 4 + 1] = axes[r].y * sc;
            inst.world[r * 4 + 2] = axes[r].z * sc;
            inst.world[r * 4 + 3] = 0.0f;
        }
        inst.world[12] = p.x;
        inst.world[13] = p.y;
        inst.world[14] = p.z;
        inst.world[15] = 1.0f;
        for (int c = 0; c < 4; ++c)
            inst.color[c] = s[ParticleAttr::Color + c][i];
        out.push_back(inst);
    }
}

void ParticleSystemInstance::ExtractLights(size_t emitter, const ParticleRendererData& renderer, std::vector<ParticleLightInstance>& out) const
{
    const ParticleEmitterState& e = emitters[emitter];
    const ParticleEmitterData& d = data->emitters[emitter];
    if (e.count == 0)
        return;
    float* const* s = e.streamPointers.data();
    const uint32_t salt = 0x1167u;
    const float* radius = EvaluateParam(emitter, renderer, salt, "Radius", 0, e.count, true, 0);
    const float* intensity = EvaluateParam(emitter, renderer, salt, "Intensity", 0, e.count, true, 1);
    const float* color = EvaluateParam(emitter, renderer, salt, "Color", 0, e.count, true, 2);
    const float* fraction = EvaluateParam(emitter, renderer, salt, "Fraction", 0, e.count, true, 3);
    const bool useParticleColor = textparse::ParseBool(ParamText(renderer, "UseParticleColor"), true);
    thread_local std::vector<float> lightOwner;
    lightOwner.clear();
    const int maxLights = std::max(0, std::atoi(ParamText(renderer, "MaxLights").c_str()));
    if (maxLights == 0)
        return;
    const size_t first = out.size();
    for (uint32_t i = 0; i < e.count; ++i)
    {
        if (s[ParticleAttr::Random][i] >= fraction[i * 4])
            continue;
        const float r = radius[i * 4];
        const float k = intensity[i * 4];
        if (r <= 0.01f || k <= 0.0f)
            continue;
        ParticleLightInstance light;
        const Vector3 p = ToWorldPoint(d, Read3(s, ParticleAttr::Position, i));
        light.position[0] = p.x;
        light.position[1] = p.y;
        light.position[2] = p.z;
        light.radius = r;
        const float alpha = useParticleColor ? std::clamp(s[ParticleAttr::Color + 3][i], 0.0f, 1.0f) : 1.0f;
        for (int c = 0; c < 3; ++c)
        {
            const float base = useParticleColor ? s[ParticleAttr::Color + c][i] : 1.0f;
            light.color[c] = std::max(0.0f, base * alpha * color[i * 4 + c] * k);
        }
        if (light.color[0] + light.color[1] + light.color[2] <= 1e-4f)
            continue;
        out.push_back(light);
        lightOwner.push_back(s[ParticleAttr::Random][i]);
    }

    // Over budget: keep the particles with the lowest random number (the same
    // ones from frame to frame, unlike array order, which changes as particles
    // die) and give them the dropped lights' energy, so the total doesn't flicker.
    const size_t count = out.size() - first;
    if (count > size_t(maxLights))
    {
        std::vector<size_t> order(count);
        for (size_t k = 0; k < count; ++k)
            order[k] = k;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lightOwner[a] < lightOwner[b]; });
        auto energy = [](const ParticleLightInstance& l) { return l.color[0] + l.color[1] + l.color[2]; };
        float total = 0.0f, kept = 0.0f;
        std::vector<ParticleLightInstance> selected;
        selected.reserve(size_t(maxLights));
        for (size_t k = 0; k < count; ++k)
        {
            const ParticleLightInstance& l = out[first + order[k]];
            total += energy(l);
            if (k < size_t(maxLights))
            {
                kept += energy(l);
                selected.push_back(l);
            }
        }
        const float energyScale = kept > 1e-6f ? total / kept : 1.0f;
        for (ParticleLightInstance& l : selected)
            for (float& c : l.color)
                c *= energyScale;
        out.resize(first);
        out.insert(out.end(), selected.begin(), selected.end());
    }
    lightOwner.clear();
}

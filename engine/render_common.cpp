#include "render_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "profiler.h"
#include "scene.h"

namespace render
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        Vector3 Normalize(const Vector3& v)
        {
            const float length = Vector3Length(v);
            return length > 1e-8f ? v / length : Vector3{0.0f, 0.0f, 1.0f};
        }

        void CopyMatrix(const Matrix4& source, float out[16])
        {
            std::memcpy(out, source.m, sizeof(source.m));
        }

        Matrix4 RotationX(float a)
        {
            Matrix4 r = MatrixIdentity();
            const float c = std::cos(a), s = std::sin(a);
            r.m[5] = c;
            r.m[6] = s;
            r.m[9] = -s;
            r.m[10] = c;
            return r;
        }

        Matrix4 RotationY(float a)
        {
            Matrix4 r = MatrixIdentity();
            const float c = std::cos(a), s = std::sin(a);
            r.m[0] = c;
            r.m[2] = -s;
            r.m[8] = s;
            r.m[10] = c;
            return r;
        }

        Matrix4 RotationZ(float a)
        {
            Matrix4 r = MatrixIdentity();
            const float c = std::cos(a), s = std::sin(a);
            r.m[0] = c;
            r.m[1] = s;
            r.m[4] = -s;
            r.m[5] = c;
            return r;
        }

        // Up vector that isn't parallel to `forward`.
        Vector3 PickUp(const Vector3& forward)
        {
            return std::fabs(forward.y) > 0.99f ? Vector3{0.0f, 0.0f, 1.0f} : Vector3{0.0f, 1.0f, 0.0f};
        }

        Vector3 LightDirection(const Light& light)
        {
            return Normalize(Vector3DirectionFromRotation(light.transform.rotation));
        }

        float Luminance(const float rgb[3])
        {
            return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
        }
    }

    // Matrices

    Matrix4 MatrixIdentity()
    {
        Matrix4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    Matrix4 MatrixMultiply(const Matrix4& a, const Matrix4& b)
    {
        Matrix4 r;
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                r.m[row * 4 + col] = a.m[row * 4 + 0] * b.m[0 * 4 + col] + a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                                     a.m[row * 4 + 2] * b.m[2 * 4 + col] + a.m[row * 4 + 3] * b.m[3 * 4 + col];
            }
        }
        return r;
    }

    Matrix4 MatrixLookToLH(const Vector3& eye, const Vector3& forward, const Vector3& up)
    {
        const Vector3 z = Normalize(forward);
        const Vector3 x = Normalize(Vector3Cross(up, z));
        const Vector3 y = Vector3Cross(z, x);
        Matrix4 r;
        r.m[0] = x.x; r.m[1] = y.x; r.m[2] = z.x;
        r.m[4] = x.y; r.m[5] = y.y; r.m[6] = z.y;
        r.m[8] = x.z; r.m[9] = y.z; r.m[10] = z.z;
        r.m[12] = -Vector3Dot(x, eye);
        r.m[13] = -Vector3Dot(y, eye);
        r.m[14] = -Vector3Dot(z, eye);
        r.m[15] = 1.0f;
        return r;
    }

    Matrix4 MatrixOrthoOffCenterLH(float left, float right, float bottom, float top, float zn, float zf)
    {
        Matrix4 r;
        r.m[0] = 2.0f / (right - left);
        r.m[5] = 2.0f / (top - bottom);
        r.m[10] = 1.0f / (zf - zn);
        r.m[12] = (left + right) / (left - right);
        r.m[13] = (top + bottom) / (bottom - top);
        r.m[14] = zn / (zn - zf);
        r.m[15] = 1.0f;
        return r;
    }

    Matrix4 MatrixPerspectiveFovLH(float fovY, float aspect, float zn, float zf)
    {
        Matrix4 r;
        const float yScale = 1.0f / std::tan(fovY * 0.5f);
        r.m[0] = yScale / aspect;
        r.m[5] = yScale;
        r.m[10] = zf / (zf - zn);
        r.m[11] = 1.0f;
        r.m[14] = -zn * zf / (zf - zn);
        return r;
    }

    Vector3 TransformPoint(const Matrix4& m, const Vector3& p, float* outW)
    {
        const float x = p.x * m.m[0] + p.y * m.m[4] + p.z * m.m[8] + m.m[12];
        const float y = p.x * m.m[1] + p.y * m.m[5] + p.z * m.m[9] + m.m[13];
        const float z = p.x * m.m[2] + p.y * m.m[6] + p.z * m.m[10] + m.m[14];
        const float w = p.x * m.m[3] + p.y * m.m[7] + p.z * m.m[11] + m.m[15];
        if (outW)
            *outW = w;
        return {x, y, z};
    }

    Matrix4 TransformToMatrix(const Transform& t)
    {
        Matrix4 r = MatrixIdentity();
        r.m[0] = t.scale.x;
        r.m[5] = t.scale.y;
        r.m[10] = t.scale.z;
        r = MatrixMultiply(r, RotationX(t.rotation.x));
        r = MatrixMultiply(r, RotationY(t.rotation.y));
        r = MatrixMultiply(r, RotationZ(t.rotation.z));
        r.m[12] += t.position.x;
        r.m[13] += t.position.y;
        r.m[14] += t.position.z;
        return r;
    }

    uint16_t FloatToHalf(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = (bits >> 16) & 0x8000u;
        int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;
        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<uint16_t>(sign);
            mantissa |= 0x800000u;
            const uint32_t shift = static_cast<uint32_t>(14 - exponent);
            return static_cast<uint16_t>(sign | ((mantissa + (1u << (shift - 1))) >> shift));
        }
        if (exponent >= 31)
            return static_cast<uint16_t>(sign | 0x7BFFu);   // clamp to the largest finite half
        const uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | ((mantissa + 0x1000u) >> 13);
        return static_cast<uint16_t>(half);
    }

    // Constants

    MaterialCBData BuildMaterialConstants(const MaterialData* material, uint32_t textureFlags)
    {
        static const MaterialData kDefault = {};
        const MaterialData& m = material ? *material : kDefault;
        MaterialCBData cb = {};
        std::memcpy(cb.baseColor, m.baseColor, sizeof(cb.baseColor));
        for (int i = 0; i < 3; ++i)
            cb.emissive[i] = m.emissiveColor[i] * m.emissiveIntensity;
        cb.emissive[3] = m.alphaCutoff;
        cb.pbr[0] = std::clamp(m.metallic, 0.0f, 1.0f);
        cb.pbr[1] = std::clamp(m.roughness, 0.02f, 1.0f);
        cb.pbr[2] = m.normalScale;
        cb.pbr[3] = std::clamp(m.occlusionStrength, 0.0f, 1.0f);
        cb.flags[0] = (m.flags & ~(MaterialFlag_HasBaseTexture | MaterialFlag_HasNormalTexture | MaterialFlag_HasMetallicRoughnessTexture |
                                   MaterialFlag_HasOcclusionTexture | MaterialFlag_HasEmissiveTexture)) |
                      textureFlags;
        return cb;
    }

    SkyCBData BuildSkyConstants(const SkySettings& sky, const Camera& camera, float aspect)
    {
        const float tanHalfFov = std::tan(kCameraFovYRadians * 0.5f);
        const float sunAngle = sky.sunSize * kPi / 180.0f;
        const Vector3 forward = CameraGetForward(camera);
        const Vector3 right = CameraGetRight(camera);
        const Vector3 up = CameraGetUp(camera);

        SkyCBData cb = {};
        auto set3 = [](float out[4], const float in[3], float w)
        {
            out[0] = in[0];
            out[1] = in[1];
            out[2] = in[2];
            out[3] = w;
        };
        auto setVec = [](float out[4], const Vector3& v)
        {
            out[0] = v.x;
            out[1] = v.y;
            out[2] = v.z;
            out[3] = 0.0f;
        };
        set3(cb.topColor, sky.topColor, 1.0f);
        set3(cb.horizonColor, sky.horizonColor, 1.0f);
        set3(cb.bottomColor, sky.bottomColor, 1.0f);
        set3(cb.sunColor, sky.sunColor, 1.0f);
        set3(cb.sunDirection, sky.sunDirection, 0.0f);
        setVec(cb.cameraForward, forward);
        setVec(cb.cameraRight, right);
        setVec(cb.cameraUp, up);
        cb.projectionParams[0] = tanHalfFov * aspect;
        cb.projectionParams[1] = tanHalfFov;
        cb.projectionParams[2] = std::cos(sunAngle);
        cb.projectionParams[3] = std::cos(sunAngle * 0.35f);
        cb.skyParams[0] = sky.sunIntensity;
        cb.skyParams[1] = sky.horizonExponent;
        cb.skyParams[2] = 1.0f;
        return cb;
    }

    // Shadows

    ShadowConfig GetShadowConfig(ShadowQuality quality)
    {
        switch (quality)
        {
        case ShadowQuality::Low: return {2, 1024, 6, 512};
        case ShadowQuality::Medium: return {3, 2048, 8, 512};
        case ShadowQuality::High: return {4, 2048, 12, 1024};
        default: return {};
        }
    }

    Frustum ExtractFrustum(const Matrix4& vp)
    {
        // clip = v * M: column j of M gives clip component j.
        auto col = [&vp](int j, int i) { return vp.m[i * 4 + j]; };
        Frustum f;
        for (int i = 0; i < 4; ++i)
        {
            f.planes[0][i] = col(3, i) + col(0, i);   // left
            f.planes[1][i] = col(3, i) - col(0, i);   // right
            f.planes[2][i] = col(3, i) + col(1, i);   // bottom
            f.planes[3][i] = col(3, i) - col(1, i);   // top
            f.planes[4][i] = col(2, i);               // near (D3D depth 0..1)
            f.planes[5][i] = col(3, i) - col(2, i);   // far
        }
        for (auto& plane : f.planes)
        {
            const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
            if (length > 1e-8f)
                for (float& value : plane)
                    value /= length;
        }
        return f;
    }

    bool SphereInFrustum(const Frustum& frustum, const Vector3& c, float radius)
    {
        for (const auto& p : frustum.planes)
        {
            if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -radius)
                return false;
        }
        return true;
    }

    namespace
    {
        struct LightCandidate
        {
            const Light* light = nullptr;
            float score = 0.0f;   // lower first
        };

        void PackLight(const Light& light, GPULight& out)
        {
            const Vector3 direction = LightDirection(light);
            const float scale = std::max(light.intensity, 0.0f) * kPi;
            out.position[0] = light.transform.position.x;
            out.position[1] = light.transform.position.y;
            out.position[2] = light.transform.position.z;
            out.position[3] = std::max(light.range, 0.01f);
            out.color[0] = light.color[0] * scale;
            out.color[1] = light.color[1] * scale;
            out.color[2] = light.color[2] * scale;
            out.direction[0] = direction.x;
            out.direction[1] = direction.y;
            out.direction[2] = direction.z;
            const float halfAngle = std::clamp(light.spotAngle, 1.0f, 179.0f) * 0.5f * kPi / 180.0f;
            out.direction[3] = std::cos(halfAngle);
            out.params[0] = static_cast<float>(light.type);
            out.params[1] = std::cos(halfAngle * 0.75f);
            out.params[2] = -1.0f;
        }

        // Stable cascade: a bounding sphere of the camera frustum slice, the
        // projection snapped to whole shadow texels so edges don't shimmer.
        Matrix4 BuildCascade(const Camera& camera, float aspect, float nearDepth, float farDepth, const Vector3& lightDir,
                             int resolution, float& outTexelSize)
        {
            const Vector3 eye = CameraGetPosition(camera);
            const Vector3 forward = CameraGetForward(camera);
            const Vector3 right = CameraGetRight(camera);
            const Vector3 up = CameraGetUp(camera);
            const float tanHalf = std::tan(kCameraFovYRadians * 0.5f);

            Vector3 corners[8];
            int index = 0;
            for (float depth : {nearDepth, farDepth})
            {
                const Vector3 center = eye + forward * depth;
                const float h = depth * tanHalf;
                const float w = h * aspect;
                for (float sy : {-1.0f, 1.0f})
                    for (float sx : {-1.0f, 1.0f})
                        corners[index++] = center + right * (w * sx) + up * (h * sy);
            }
            Vector3 center = {};
            for (const Vector3& corner : corners)
                center += corner;
            center = center / 8.0f;
            float radius = 0.0f;
            for (const Vector3& corner : corners)
                radius = std::max(radius, Vector3Length(corner - center));
            radius = std::ceil(radius * 16.0f) / 16.0f;   // stable size as the camera turns

            constexpr float kCasterExtend = 150.0f;   // casters behind the slice (towers, terrain)
            const Vector3 shadowEye = center - lightDir * (radius + kCasterExtend);
            const Matrix4 view = MatrixLookToLH(shadowEye, lightDir, PickUp(lightDir));
            const Matrix4 proj = MatrixOrthoOffCenterLH(-radius, radius, -radius, radius, 0.0f, radius * 2.0f + kCasterExtend);
            Matrix4 viewProj = MatrixMultiply(view, proj);

            // Snap the world origin to a texel; moving the camera then shifts
            // the projection by whole texels only.
            const float halfRes = static_cast<float>(resolution) * 0.5f;
            const Vector3 origin = TransformPoint(viewProj, {0.0f, 0.0f, 0.0f});
            const float x = origin.x * halfRes;
            const float y = origin.y * halfRes;
            viewProj.m[12] += (std::round(x) - x) / halfRes;
            viewProj.m[13] += (std::round(y) - y) / halfRes;

            outTexelSize = radius * 2.0f / static_cast<float>(resolution);
            return viewProj;
        }
    }

    void BuildFrameSetup(FrameSetup& out, const SceneState& state, const Camera& camera, const Scene& scene,
                         float aspect, const GraphicsOptions& options)
    {
        KES_ZONE_FN(RENDER);
        out.shadowViews.clear();
        out.shadows = GetShadowConfig(options.shadowQuality);
        SceneCBData& cb = out.scene;
        // Keep the SH (written by ApplyEnvironment), reset the rest.
        float sh[9][4];
        std::memcpy(sh, cb.ambientSH, sizeof(sh));
        const float environmentParams[4] = {cb.environmentParams[0], cb.environmentParams[1], cb.environmentParams[2], cb.environmentParams[3]};
        cb = {};
        std::memcpy(cb.ambientSH, sh, sizeof(sh));
        std::memcpy(cb.environmentParams, environmentParams, sizeof(environmentParams));

        CameraViewData cameraData = {};
        CameraBuildMatrices(camera, aspect, cameraData);
        std::memcpy(out.viewProj.m, cameraData.viewProjection, sizeof(out.viewProj.m));
        CopyMatrix(out.viewProj, cb.viewProj);
        out.frustum = ExtractFrustum(out.viewProj);

        const Vector3 eye = CameraGetPosition(camera);
        const Vector3 forward = CameraGetForward(camera);
        cb.cameraPosition[0] = eye.x;
        cb.cameraPosition[1] = eye.y;
        cb.cameraPosition[2] = eye.z;
        cb.cameraForward[0] = forward.x;
        cb.cameraForward[1] = forward.y;
        cb.cameraForward[2] = forward.z;

        // Lights: directional first, then visible local lights nearest first.
        std::vector<LightCandidate> candidates;
        candidates.reserve(scene.GetLights().size());
        for (const Light& light : scene.GetLights())
        {
            if (!light.active || light.intensity <= 0.0f || Luminance(light.color) <= 0.0f)
                continue;
            if (light.type == LightType::Directional)
            {
                candidates.push_back({&light, -1e30f});
                continue;
            }
            const Vector3 position = light.transform.position;
            if (!SphereInFrustum(out.frustum, position, light.range))
                continue;
            candidates.push_back({&light, Vector3Length(position - eye) - light.range});
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const LightCandidate& a, const LightCandidate& b) { return a.score < b.score; });
        if (candidates.size() > static_cast<size_t>(kMaxLights))
            candidates.resize(kMaxLights);

        int lightCount = 0;
        bool sunAssigned = false;
        int nextLocalSlice = 0;
        for (const LightCandidate& candidate : candidates)
        {
            const Light& light = *candidate.light;
            GPULight& gpu = cb.lights[lightCount++];
            PackLight(light, gpu);
            if (!light.castShadows || out.shadows.cascadeCount == 0)
                continue;

            if (light.type == LightType::Directional)
            {
                if (sunAssigned)
                    continue;
                sunAssigned = true;
                gpu.params[2] = 1.0f;

                const Vector3 lightDir = LightDirection(light);
                const float nearPlane = kCameraNearPlane;
                const float farPlane = std::clamp(options.shadowDistance, 5.0f, kCameraFarPlane);
                const int count = out.shadows.cascadeCount;
                float splits[kMaxCascades + 1] = {nearPlane};
                for (int i = 1; i <= count; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(count);
                    const float logSplit = nearPlane * std::pow(farPlane / nearPlane, t);
                    const float linearSplit = nearPlane + (farPlane - nearPlane) * t;
                    splits[i] = 0.8f * logSplit + 0.2f * linearSplit;
                }
                for (int i = 0; i < count; ++i)
                {
                    float texel = 0.0f;
                    ShadowView view;
                    view.viewProj = BuildCascade(camera, aspect, splits[i], splits[i + 1], lightDir, out.shadows.cascadeResolution, texel);
                    view.slice = i;
                    view.cascade = true;
                    out.shadowViews.push_back(view);
                    CopyMatrix(view.viewProj, cb.cascadeViewProj[i]);
                    cb.cascadeSplits[i] = splits[i + 1];
                    cb.cascadeTexelSize[i] = texel;
                }
                cb.shadowParams[0] = static_cast<float>(count);
                continue;
            }

            // Local shadows: a spot uses one slice, a point light six (one per cube face).
            const int needed = light.type == LightType::Point ? 6 : 1;
            if (nextLocalSlice + needed > out.shadows.localSlices)
                continue;
            gpu.params[2] = static_cast<float>(nextLocalSlice);
            const Vector3 position = light.transform.position;
            const float range = std::max(light.range, 0.1f);
            const float zn = std::max(0.05f, range * 0.002f);
            if (light.type == LightType::Spot)
            {
                const Vector3 dir = LightDirection(light);
                const float fov = std::min(std::clamp(light.spotAngle, 1.0f, 179.0f) * kPi / 180.0f + 0.1f, 3.0f);
                ShadowView view;
                view.viewProj = MatrixMultiply(MatrixLookToLH(position, dir, PickUp(dir)), MatrixPerspectiveFovLH(fov, 1.0f, zn, range));
                view.slice = nextLocalSlice;
                view.origin = position;
                view.radius = range;
                out.shadowViews.push_back(view);
                CopyMatrix(view.viewProj, cb.localShadowViewProj[nextLocalSlice]);
            }
            else
            {
                // Faces +X -X +Y -Y +Z -Z; the shader picks one by the major axis.
                // 92 degrees so filtering near face edges stays inside the slice.
                static const Vector3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                static const Vector3 kUps[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
                const Matrix4 proj = MatrixPerspectiveFovLH(92.0f * kPi / 180.0f, 1.0f, zn, range);
                for (int face = 0; face < 6; ++face)
                {
                    ShadowView view;
                    view.viewProj = MatrixMultiply(MatrixLookToLH(position, kDirs[face], kUps[face]), proj);
                    view.slice = nextLocalSlice + face;
                    view.origin = position;
                    view.radius = range;
                    out.shadowViews.push_back(view);
                    CopyMatrix(view.viewProj, cb.localShadowViewProj[nextLocalSlice + face]);
                }
            }
            nextLocalSlice += needed;
        }
        cb.lightCounts[0] = static_cast<float>(lightCount);
        if (out.shadows.cascadeCount > 0)
        {
            cb.shadowParams[1] = 1.0f / static_cast<float>(out.shadows.cascadeResolution);
            cb.shadowParams[2] = 1.0f / static_cast<float>(std::max(out.shadows.localResolution, 1));
            cb.shadowParams[3] = 1.0f;
        }

        const FogSettings& fog = scene.GetFog();
        cb.fogColor[0] = fog.color[0];
        cb.fogColor[1] = fog.color[1];
        cb.fogColor[2] = fog.color[2];
        cb.fogParams[0] = fog.nearDistance;
        cb.fogParams[1] = std::max(fog.farDistance, fog.nearDistance + 0.01f);
        cb.fogParams[2] = fog.active ? 1.0f : 0.0f;
        cb.fogParams[3] = 1.0f;
        (void)state;
    }

    // Environment (IBL)

    namespace
    {
        struct EnvironmentSource
        {
            bool sky = false;
            float top[3], horizon[3], bottom[3];
            float exponent = 1.0f;
            float scale = 1.0f;
            float flat[3];

            // Radiance seen in direction `d`. Matches sky.slang without the
            // sun disc: the sun is a light of its own.
            void Sample(const Vector3& d, float out[3]) const
            {
                if (!sky)
                {
                    out[0] = flat[0];
                    out[1] = flat[1];
                    out[2] = flat[2];
                    return;
                }
                const float blend = std::pow(std::clamp(std::fabs(d.y), 0.0f, 1.0f), std::max(exponent, 0.01f));
                const float* target = d.y >= 0.0f ? top : bottom;
                for (int i = 0; i < 3; ++i)
                    out[i] = (horizon[i] + (target[i] - horizon[i]) * blend) * scale;
            }
        };

        EnvironmentSource MakeSource(const SceneState& state, const Scene& scene)
        {
            EnvironmentSource source;
            const SkySettings& sky = scene.GetSky();
            source.sky = sky.active;
            for (int i = 0; i < 3; ++i)
            {
                source.top[i] = sky.topColor[i];
                source.horizon[i] = sky.horizonColor[i];
                source.bottom[i] = sky.bottomColor[i];
                source.flat[i] = state.ambient[i];
            }
            source.exponent = sky.horizonExponent;
            source.scale = std::max(sky.ambientIntensity, 0.0f);
            return source;
        }

        Vector3 CubeDirection(int face, float u, float v)
        {
            switch (face)
            {
            case 0: return Normalize({1.0f, -v, -u});
            case 1: return Normalize({-1.0f, -v, u});
            case 2: return Normalize({u, 1.0f, v});
            case 3: return Normalize({u, -1.0f, -v});
            case 4: return Normalize({u, -v, 1.0f});
            default: return Normalize({-u, -v, -1.0f});
            }
        }

        Vector3 ImportanceSampleGGX(float xi0, float xi1, float roughness, const Vector3& n)
        {
            const float a = roughness * roughness;
            const float phi = 2.0f * kPi * xi0;
            const float cosTheta = std::sqrt((1.0f - xi1) / (1.0f + (a * a - 1.0f) * xi1));
            const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const Vector3 h = {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
            const Vector3 up = std::fabs(n.z) < 0.999f ? Vector3{0, 0, 1} : Vector3{1, 0, 0};
            const Vector3 tx = Normalize(Vector3Cross(up, n));
            const Vector3 ty = Vector3Cross(n, tx);
            return Normalize(tx * h.x + ty * h.y + n * h.z);
        }

        float RadicalInverse(uint32_t bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        }

        uint64_t Hash(uint64_t hash, const void* data, size_t size)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i)
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            return hash;
        }
    }

    uint64_t EnvironmentKey(const SceneState& state, const Scene& scene)
    {
        const EnvironmentSource source = MakeSource(state, scene);
        uint64_t hash = 1469598103934665603ull;
        hash = Hash(hash, &source.sky, sizeof(source.sky));
        if (source.sky)
        {
            hash = Hash(hash, source.top, sizeof(source.top));
            hash = Hash(hash, source.horizon, sizeof(source.horizon));
            hash = Hash(hash, source.bottom, sizeof(source.bottom));
            hash = Hash(hash, &source.exponent, sizeof(source.exponent));
            hash = Hash(hash, &source.scale, sizeof(source.scale));
        }
        else
        {
            hash = Hash(hash, source.flat, sizeof(source.flat));
        }
        return hash | 1u;   // never 0 ("not built")
    }

    void BuildEnvironment(const SceneState& state, const Scene& scene, EnvironmentMaps& out)
    {
        KES_ZONE_FN(RENDER);
        const EnvironmentSource source = MakeSource(state, scene);
        out.key = EnvironmentKey(state, scene);
        out.faceSize = kEnvironmentFaceSize;
        out.mipCount = kEnvironmentMipCount;

        // The source only depends on the direction's y (a gradient around the
        // vertical axis), so every mip is a function of y as well: prefilter a
        // table over y and fill the faces from it.
        constexpr int kTableSize = 256;
        constexpr int kSamples = 256;
        std::vector<std::array<float, 3>> table(kTableSize);
        for (int mip = 0; mip < kEnvironmentMipCount; ++mip)
        {
            const float roughness = static_cast<float>(mip) / static_cast<float>(kEnvironmentMipCount - 1);
            for (int k = 0; k < kTableSize; ++k)
            {
                const float y = -1.0f + 2.0f * static_cast<float>(k) / static_cast<float>(kTableSize - 1);
                const Vector3 n = Normalize({std::sqrt(std::max(0.0f, 1.0f - y * y)), y, 0.0f});
                float sum[3] = {};
                if (mip == 0 || !source.sky)
                {
                    source.Sample(n, sum);
                }
                else
                {
                    float weight = 0.0f;
                    for (int s = 0; s < kSamples; ++s)
                    {
                        const Vector3 h = ImportanceSampleGGX((static_cast<float>(s) + 0.5f) / kSamples, RadicalInverse(static_cast<uint32_t>(s)), roughness, n);
                        const Vector3 l = h * (2.0f * Vector3Dot(n, h)) - n;
                        const float ndotl = Vector3Dot(n, l);
                        if (ndotl <= 0.0f)
                            continue;
                        float radiance[3];
                        source.Sample(l, radiance);
                        for (int c = 0; c < 3; ++c)
                            sum[c] += radiance[c] * ndotl;
                        weight += ndotl;
                    }
                    for (float& value : sum)
                        value /= std::max(weight, 1e-6f);
                }
                table[k] = {sum[0], sum[1], sum[2]};
            }

            const int size = std::max(kEnvironmentFaceSize >> mip, 1);
            for (int face = 0; face < 6; ++face)
            {
                std::vector<uint16_t>& texels = out.faces[mip][face];
                texels.resize(static_cast<size_t>(size) * size * 4);
                for (int py = 0; py < size; ++py)
                {
                    for (int px = 0; px < size; ++px)
                    {
                        const float u = 2.0f * (static_cast<float>(px) + 0.5f) / size - 1.0f;
                        const float v = 2.0f * (static_cast<float>(py) + 0.5f) / size - 1.0f;
                        const Vector3 d = CubeDirection(face, u, v);
                        const float t = (std::clamp(d.y, -1.0f, 1.0f) + 1.0f) * 0.5f * (kTableSize - 1);
                        const int k0 = std::min(static_cast<int>(t), kTableSize - 2);
                        const float f = t - static_cast<float>(k0);
                        uint16_t* dst = &texels[(static_cast<size_t>(py) * size + px) * 4];
                        for (int c = 0; c < 3; ++c)
                            dst[c] = FloatToHalf(table[k0][c] + (table[k0 + 1][c] - table[k0][c]) * f);
                        dst[3] = FloatToHalf(1.0f);
                    }
                }
            }
        }

        // Diffuse: project onto 9 SH coefficients, convolve with the cosine
        // lobe and divide by pi, so the shader gets irradiance / pi directly.
        double coefficients[9][3] = {};
        double totalWeight = 0.0;
        constexpr int kShSize = 32;
        for (int face = 0; face < 6; ++face)
        {
            for (int py = 0; py < kShSize; ++py)
            {
                for (int px = 0; px < kShSize; ++px)
                {
                    const float u = 2.0f * (static_cast<float>(px) + 0.5f) / kShSize - 1.0f;
                    const float v = 2.0f * (static_cast<float>(py) + 0.5f) / kShSize - 1.0f;
                    const Vector3 d = CubeDirection(face, u, v);
                    const double weight = 1.0 / std::pow(1.0 + u * u + v * v, 1.5);
                    float radiance[3];
                    source.Sample(d, radiance);
                    const double basis[9] = {
                        0.282095,
                        0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x,
                        1.092548 * d.x * d.y, 1.092548 * d.y * d.z, 0.315392 * (3.0 * d.z * d.z - 1.0),
                        1.092548 * d.x * d.z, 0.546274 * (d.x * d.x - d.y * d.y)};
                    for (int i = 0; i < 9; ++i)
                        for (int c = 0; c < 3; ++c)
                            coefficients[i][c] += radiance[c] * basis[i] * weight;
                    totalWeight += weight;
                }
            }
        }
        const double normalize = 4.0 * kPi / totalWeight;
        const double band[9] = {1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};   // A_l / pi
        for (int i = 0; i < 9; ++i)
        {
            for (int c = 0; c < 3; ++c)
                out.sh[i][c] = static_cast<float>(coefficients[i][c] * normalize * band[i]);
            out.sh[i][3] = 0.0f;
        }
    }

    void ApplyEnvironment(const EnvironmentMaps& environment, SceneCBData& scene)
    {
        std::memcpy(scene.ambientSH, environment.sh, sizeof(scene.ambientSH));
        scene.environmentParams[0] = static_cast<float>(environment.mipCount);
        scene.environmentParams[1] = 1.0f;
    }

    // Mips

    namespace
    {
        const float* SrgbToLinearTable()
        {
            static float table[256];
            static bool built = false;
            if (!built)
            {
                for (int i = 0; i < 256; ++i)
                {
                    const float c = static_cast<float>(i) / 255.0f;
                    table[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                }
                built = true;
            }
            return table;
        }

        uint8_t LinearToSrgb(float c)
        {
            c = std::clamp(c, 0.0f, 1.0f);
            const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            return static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
        }

        uint8_t ToByte(float c)
        {
            return static_cast<uint8_t>(std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }

    void GenerateMips(const uint8_t* rgba, uint32_t width, uint32_t height, TextureUsage usage, std::vector<MipLevel>& outLevels)
    {
        KES_ZONE_FN(RENDER);
        outLevels.clear();
        if (!rgba || width == 0 || height == 0)
            return;
        MipLevel base;
        base.width = width;
        base.height = height;
        base.pixels.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
        outLevels.push_back(std::move(base));

        const float* toLinear = SrgbToLinearTable();
        while (outLevels.back().width > 1 || outLevels.back().height > 1)
        {
            const MipLevel& src = outLevels.back();
            MipLevel dst;
            dst.width = std::max(src.width / 2, 1u);
            dst.height = std::max(src.height / 2, 1u);
            dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * 4);
            for (uint32_t y = 0; y < dst.height; ++y)
            {
                for (uint32_t x = 0; x < dst.width; ++x)
                {
                    float sum[4] = {};
                    for (uint32_t dy = 0; dy < 2; ++dy)
                    {
                        for (uint32_t dx = 0; dx < 2; ++dx)
                        {
                            const uint32_t sx = std::min(x * 2 + dx, src.width - 1);
                            const uint32_t sy = std::min(y * 2 + dy, src.height - 1);
                            const uint8_t* p = &src.pixels[(static_cast<size_t>(sy) * src.width + sx) * 4];
                            for (int c = 0; c < 3; ++c)
                            {
                                if (usage == TextureUsage::Color)
                                    sum[c] += toLinear[p[c]];
                                else if (usage == TextureUsage::Normal)
                                    sum[c] += static_cast<float>(p[c]) / 127.5f - 1.0f;
                                else
                                    sum[c] += static_cast<float>(p[c]) / 255.0f;
                            }
                            sum[3] += static_cast<float>(p[3]) / 255.0f;
                        }
                    }
                    uint8_t* out = &dst.pixels[(static_cast<size_t>(y) * dst.width + x) * 4];
                    if (usage == TextureUsage::Normal)
                    {
                        Vector3 n = Normalize({sum[0], sum[1], sum[2]});
                        out[0] = ToByte(n.x * 0.5f + 0.5f);
                        out[1] = ToByte(n.y * 0.5f + 0.5f);
                        out[2] = ToByte(n.z * 0.5f + 0.5f);
                    }
                    else
                    {
                        for (int c = 0; c < 3; ++c)
                            out[c] = usage == TextureUsage::Color ? LinearToSrgb(sum[c] * 0.25f) : ToByte(sum[c] * 0.25f);
                    }
                    out[3] = ToByte(sum[3] * 0.25f);
                }
            }
            outLevels.push_back(std::move(dst));
        }
    }

    // Bounds

    void ComputeBoundingSphere(const void* vertices, size_t count, size_t stride, Vector3& outCenter, float& outRadius)
    {
        outCenter = {};
        outRadius = 0.0f;
        if (!vertices || count == 0)
            return;
        const uint8_t* bytes = static_cast<const uint8_t*>(vertices);
        auto position = [&](size_t i)
        {
            float p[3];
            std::memcpy(p, bytes + i * stride, sizeof(p));
            return Vector3{p[0], p[1], p[2]};
        };
        Vector3 minV = position(0), maxV = minV;
        for (size_t i = 1; i < count; ++i)
        {
            const Vector3 p = position(i);
            minV = {std::min(minV.x, p.x), std::min(minV.y, p.y), std::min(minV.z, p.z)};
            maxV = {std::max(maxV.x, p.x), std::max(maxV.y, p.y), std::max(maxV.z, p.z)};
        }
        outCenter = (minV + maxV) * 0.5f;
        float radiusSq = 0.0f;
        for (size_t i = 0; i < count; ++i)
            radiusSq = std::max(radiusSq, Vector3LengthSq(position(i) - outCenter));
        outRadius = std::sqrt(radiusSq);
    }

    void TransformSphere(const Transform& transform, const Vector3& localCenter, float localRadius, Vector3& outCenter, float& outRadius)
    {
        outCenter = TransformPoint(TransformToMatrix(transform), localCenter);
        const float scale = std::max({std::fabs(transform.scale.x), std::fabs(transform.scale.y), std::fabs(transform.scale.z)});
        outRadius = localRadius * scale;
    }
}

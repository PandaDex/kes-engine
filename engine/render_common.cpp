#include "render_common.h"

#include <algorithm>
#include <array>
#include <chrono>
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
            return length > 1e-8f ? v / length : Vector3{ 0.0f, 0.0f, 1.0f };
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
            return std::fabs(forward.y) > 0.99f ? Vector3{ 0.0f, 0.0f, 1.0f } : Vector3{ 0.0f, 1.0f, 0.0f };
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
        r.m[0] = x.x;
        r.m[1] = y.x;
        r.m[2] = z.x;
        r.m[4] = x.y;
        r.m[5] = y.y;
        r.m[6] = z.y;
        r.m[8] = x.z;
        r.m[9] = y.z;
        r.m[10] = z.z;
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
        return { x, y, z };
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

    MaterialHandle SectionMaterial(const SceneObject& object, int32_t materialSlot)
    {
        if (materialSlot >= 0 && (size_t)materialSlot < object.slotMaterials.size() && object.slotMaterials[(size_t)materialSlot] != 0)
            return object.slotMaterials[(size_t)materialSlot];
        return object.material;
    }

    Matrix4 SceneObjectMatrix(const SceneObject& object, Vector3& outScale)
    {
        if (!object.useWorldMatrix)
        {
            outScale = object.transform.scale;
            return TransformToMatrix(object.transform);
        }
        Matrix4 r;
        std::memcpy(r.m, object.worldMatrix, sizeof(r.m));
        const auto rowLength = [&](int row)
        { return std::sqrt(r.m[row * 4] * r.m[row * 4] + r.m[row * 4 + 1] * r.m[row * 4 + 1] + r.m[row * 4 + 2] * r.m[row * 4 + 2]); };
        outScale = { rowLength(0), rowLength(1), rowLength(2) };
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
            return static_cast<uint16_t>(sign | 0x7BFFu); // clamp to the largest finite half
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
        cb.flags[0] = (m.flags & ~(MaterialFlag_HasBaseTexture | MaterialFlag_HasNormalTexture | MaterialFlag_HasMetallicRoughnessTexture | MaterialFlag_HasOcclusionTexture | MaterialFlag_HasEmissiveTexture)) |
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
        case ShadowQuality::Low:    return { 2, 1024, 2048, 512 };
        case ShadowQuality::Medium: return { 3, 2048, 4096, 512 };
        case ShadowQuality::High:   return { 4, 2048, 4096, 1024 };
        default:                    return {};
        }
    }

    Frustum ExtractFrustum(const Matrix4& vp)
    {
        // clip = v * M: column j of M gives clip component j.
        auto col = [&vp](int j, int i)
        { return vp.m[i * 4 + j]; };
        Frustum f;
        for (int i = 0; i < 4; ++i)
        {
            f.planes[0][i] = col(3, i) + col(0, i); // left
            f.planes[1][i] = col(3, i) - col(0, i); // right
            f.planes[2][i] = col(3, i) + col(1, i); // bottom
            f.planes[3][i] = col(3, i) - col(1, i); // top
            f.planes[4][i] = col(2, i); // near (D3D depth 0..1)
            f.planes[5][i] = col(3, i) - col(2, i); // far
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
            float score = 0.0f; // lower first
            Vector3 center;
            float radius = 0.0f;
        };

        // A light waiting to be binned, with the sphere it can reach.
        struct ClusterCandidate
        {
            GPULight gpu;
            Vector3 center;
            float radius = 0.0f;
            const Light* source = nullptr; // scene lights only
        };

        // Range sphere, or for a spot the sphere through its apex and cap rim
        // (center R / (2 cos half) along the axis) when that one is smaller.
        void LocalLightBounds(const Light& light, Vector3& outCenter, float& outRadius)
        {
            const float range = std::max(light.range, 0.01f);
            outCenter = light.transform.position;
            outRadius = range;
            if (light.type != LightType::Spot)
                return;
            const float cosHalf = std::cos(std::clamp(light.spotAngle, 1.0f, 179.0f) * 0.5f * kPi / 180.0f);
            const float radius = range / (2.0f * cosHalf);
            if (radius >= range)
                return;
            outCenter = light.transform.position + LightDirection(light) * radius;
            outRadius = radius;
        }

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
            for (float depth : { nearDepth, farDepth })
            {
                const Vector3 center = eye + forward * depth;
                const float h = depth * tanHalf;
                const float w = h * aspect;
                for (float sy : { -1.0f, 1.0f })
                    for (float sx : { -1.0f, 1.0f })
                        corners[index++] = center + right * (w * sx) + up * (h * sy);
            }
            Vector3 center = {};
            for (const Vector3& corner : corners)
                center += corner;
            center = center / 8.0f;
            float radius = 0.0f;
            for (const Vector3& corner : corners)
                radius = std::max(radius, Vector3Length(corner - center));
            radius = std::ceil(radius * 16.0f) / 16.0f; // stable size as the camera turns

            constexpr float kCasterExtend = 150.0f; // casters behind the slice (towers, terrain)
            const Vector3 shadowEye = center - lightDir * (radius + kCasterExtend);
            const Matrix4 view = MatrixLookToLH(shadowEye, lightDir, PickUp(lightDir));
            const Matrix4 proj = MatrixOrthoOffCenterLH(-radius, radius, -radius, radius, 0.0f, radius * 2.0f + kCasterExtend);
            Matrix4 viewProj = MatrixMultiply(view, proj);

            // Snap the world origin to a texel; moving the camera then shifts
            // the projection by whole texels only.
            const float halfRes = static_cast<float>(resolution) * 0.5f;
            const Vector3 origin = TransformPoint(viewProj, { 0.0f, 0.0f, 0.0f });
            const float x = origin.x * halfRes;
            const float y = origin.y * halfRes;
            viewProj.m[12] += (std::round(x) - x) / halfRes;
            viewProj.m[13] += (std::round(y) - y) / halfRes;

            outTexelSize = radius * 2.0f / static_cast<float>(resolution);
            return viewProj;
        }
    }

    // Bins local lights into the cluster grid, in order until kMaxClusterLights.
    // View space comes from the camera's own view matrix so it matches what the
    // shader derives from uViewProj and the camera forward.
    static void BuildClusters(FrameSetup& out, const CameraViewData& camera, const std::vector<ClusterCandidate>& candidates,
        std::vector<const Light*>& outSources)
    {
        KES_ZONE_FN(RENDER);
        out.clusterLights.clear();
        outSources.clear();
        out.clusterGrid.assign(static_cast<size_t>(kClusterCount), 0u);
        const float nearZ = kCameraNearPlane;
        const float farZ = kCameraFarPlane;
        const float sliceScale = static_cast<float>(kClusterZ) / std::log(farZ / nearZ);
        out.scene.clusterParams[0] = static_cast<float>(kClusterX);
        out.scene.clusterParams[1] = static_cast<float>(kClusterY);
        out.scene.clusterParams[2] = static_cast<float>(kClusterZ);
        out.scene.clusterDepth[0] = nearZ;
        out.scene.clusterDepth[1] = sliceScale;
        if (candidates.empty())
            return;

        const float* v = camera.view;
        const float xScale = camera.projection[0];
        const float yScale = camera.projection[5];
        auto slice = [&](float z)
        { return std::clamp(static_cast<int>(std::floor(std::log(std::max(z, nearZ) / nearZ) * sliceScale)), 0, kClusterZ - 1); };
        auto tile = [](float ndc, int count)
        { return std::clamp(static_cast<int>(std::floor((ndc * 0.5f + 0.5f) * static_cast<float>(count))), 0, count - 1); };

        struct Range
        {
            int x0, x1, y0, y1, z0, z1;
        };
        std::vector<Range> ranges;
        ranges.reserve(std::min<size_t>(candidates.size(), kMaxClusterLights));
        out.clusterLights.reserve(ranges.capacity());
        for (const ClusterCandidate& candidate : candidates)
        {
            if (out.clusterLights.size() >= static_cast<size_t>(kMaxClusterLights))
                break;
            const Vector3& p = candidate.center;
            const float r = candidate.radius;
            if (r <= 0.0f || !SphereInFrustum(out.frustum, p, r))
                continue;
            const float vx = p.x * v[0] + p.y * v[4] + p.z * v[8] + v[12];
            const float vy = p.x * v[1] + p.y * v[5] + p.z * v[9] + v[13];
            const float vz = p.x * v[2] + p.y * v[6] + p.z * v[10] + v[14];
            const float zMin = std::max(vz - r, nearZ);
            const float zMax = vz + r;
            if (zMax < nearZ || zMin > farZ)
                continue;

            // x/z and y/z are monotonic in each variable for z > 0, so the
            // box corners bound the projected sphere.
            float nx0 = 1e30f, nx1 = -1e30f, ny0 = 1e30f, ny1 = -1e30f;
            for (int iz = 0; iz < 2; ++iz)
            {
                const float z = iz ? zMax : zMin;
                for (int i = 0; i < 2; ++i)
                {
                    const float x = (vx + (i ? r : -r)) * xScale / z;
                    const float y = (vy + (i ? r : -r)) * yScale / z;
                    nx0 = std::min(nx0, x);
                    nx1 = std::max(nx1, x);
                    ny0 = std::min(ny0, y);
                    ny1 = std::max(ny1, y);
                }
            }
            if (nx1 < -1.0f || nx0 > 1.0f || ny1 < -1.0f || ny0 > 1.0f)
                continue;

            ranges.push_back({ tile(nx0, kClusterX), tile(nx1, kClusterX), tile(ny0, kClusterY), tile(ny1, kClusterY), slice(zMin), slice(zMax) });
            out.clusterLights.push_back(candidate.gpu);
            outSources.push_back(candidate.source);
        }
        out.scene.clusterParams[3] = static_cast<float>(out.clusterLights.size());
        if (out.clusterLights.empty())
            return;

        std::vector<uint32_t> counts(static_cast<size_t>(kClusterCount), 0u);
        for (const Range& range : ranges)
            for (int z = range.z0; z <= range.z1; ++z)
                for (int y = range.y0; y <= range.y1; ++y)
                    for (int x = range.x0; x <= range.x1; ++x)
                    {
                        uint32_t& count = counts[static_cast<size_t>((z * kClusterY + y) * kClusterX + x)];
                        count = std::min(count + 1u, kMaxLightsPerCluster);
                    }

        uint32_t offset = 0;
        for (int c = 0; c < kClusterCount; ++c)
        {
            uint32_t count = counts[static_cast<size_t>(c)];
            if (offset + count > static_cast<uint32_t>(kMaxClusterIndices))
                count = static_cast<uint32_t>(kMaxClusterIndices) - offset;
            out.clusterGrid[static_cast<size_t>(c)] = offset << 8; // count is filled in below
            counts[static_cast<size_t>(c)] = count; // capacity
            offset += count;
        }
        out.clusterGrid.resize(static_cast<size_t>(kClusterCount) + offset, 0u);

        uint32_t* grid = out.clusterGrid.data();
        for (uint32_t li = 0; li < static_cast<uint32_t>(ranges.size()); ++li)
        {
            const Range& range = ranges[li];
            for (int z = range.z0; z <= range.z1; ++z)
                for (int y = range.y0; y <= range.y1; ++y)
                    for (int x = range.x0; x <= range.x1; ++x)
                    {
                        const size_t c = static_cast<size_t>((z * kClusterY + y) * kClusterX + x);
                        const uint32_t filled = grid[c] & 0xFFu;
                        if (filled >= counts[c])
                            continue;
                        grid[kClusterCount + (grid[c] >> 8) + filled] = li;
                        grid[c] += 1u;
                    }
        }
    }

    // Local shadow atlas

    void InvalidateShadowCache(ShadowAtlasCache& cache)
    {
        cache.entries.clear();
        cache.frameEntries.clear();
        std::fill(cache.cells.begin(), cache.cells.end(), uint8_t(0));
        cache.casters.clear();
    }

    namespace
    {
        constexpr float kShadowReferenceHeight = 1080.0f; // tile size: on-screen diameter at this height
        constexpr int kShadowRefreshBudget = 16; // tiles redrawn per frame because a caster moved

        int AtlasCells(const ShadowAtlasCache& cache)
        {
            return cache.atlasSize / kMinShadowTile;
        }

        void MarkTile(ShadowAtlasCache& cache, int x, int y, int size, uint8_t value)
        {
            const int cells = AtlasCells(cache);
            const int k = size / kMinShadowTile;
            for (int cy = y / kMinShadowTile; cy < y / kMinShadowTile + k; ++cy)
                for (int cx = x / kMinShadowTile; cx < x / kMinShadowTile + k; ++cx)
                    cache.cells[static_cast<size_t>(cy * cells + cx)] = value;
        }

        // First free block aligned to its own size.
        bool AllocateTile(ShadowAtlasCache& cache, int size, int& outX, int& outY)
        {
            const int cells = AtlasCells(cache);
            const int k = size / kMinShadowTile;
            for (int cy = 0; cy + k <= cells; cy += k)
                for (int cx = 0; cx + k <= cells; cx += k)
                {
                    bool free = true;
                    for (int y = cy; y < cy + k && free; ++y)
                        for (int x = cx; x < cx + k && free; ++x)
                            free = cache.cells[static_cast<size_t>(y * cells + x)] == 0;
                    if (!free)
                        continue;
                    outX = cx * kMinShadowTile;
                    outY = cy * kMinShadowTile;
                    MarkTile(cache, outX, outY, size, 1);
                    return true;
                }
            return false;
        }

        void FreeEntryTiles(ShadowAtlasCache& cache, ShadowAtlasCache::Entry& entry)
        {
            for (int i = 0; i < entry.tileCount; ++i)
                MarkTile(cache, entry.x[i], entry.y[i], entry.tileSize, 0);
            entry.tileCount = 0;
            entry.contentValid = false;
        }

        // `count` tiles of `size`, evicting the least recently used entries
        // that aren't drawn this frame when the atlas is full.
        bool AllocateTiles(ShadowAtlasCache& cache, int size, int count, int outX[6], int outY[6])
        {
            for (;;)
            {
                int placed = 0;
                while (placed < count && AllocateTile(cache, size, outX[placed], outY[placed]))
                    ++placed;
                if (placed == count)
                    return true;
                for (int i = 0; i < placed; ++i)
                    MarkTile(cache, outX[i], outY[i], size, 0);

                int victim = -1;
                for (int i = 0; i < static_cast<int>(cache.entries.size()); ++i)
                {
                    const ShadowAtlasCache::Entry& e = cache.entries[static_cast<size_t>(i)];
                    if (e.tileCount == 0 || e.lastUsedFrame == cache.frame)
                        continue;
                    if (victim < 0 || e.lastUsedFrame < cache.entries[static_cast<size_t>(victim)].lastUsedFrame)
                        victim = i;
                }
                if (victim < 0)
                    return false;
                FreeEntryTiles(cache, cache.entries[static_cast<size_t>(victim)]);
            }
        }

        uint64_t LightShadowState(const Light& light)
        {
            const float values[] = { static_cast<float>(light.type), light.transform.position.x, light.transform.position.y,
                light.transform.position.z, light.transform.rotation.x, light.transform.rotation.y, light.transform.rotation.z,
                light.range, light.spotAngle };
            uint64_t h = 1469598103934665603ull;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(values);
            for (size_t i = 0; i < sizeof(values); ++i)
                h = (h ^ p[i]) * 1099511628211ull;
            return h;
        }

        // A spot's one view or a point light's six (+X -X +Y -Y +Z -Z; the
        // shader picks a face by the major axis). 92 degrees so filtering near
        // face edges stays inside the tile.
        int LocalShadowMatrices(const Light& light, Matrix4 out[6])
        {
            const Vector3 position = light.transform.position;
            const float range = std::max(light.range, 0.1f);
            const float zn = std::max(0.05f, range * 0.002f);
            if (light.type == LightType::Spot)
            {
                const Vector3 dir = LightDirection(light);
                const float fov = std::min(std::clamp(light.spotAngle, 1.0f, 179.0f) * kPi / 180.0f + 0.1f, 3.0f);
                out[0] = MatrixMultiply(MatrixLookToLH(position, dir, PickUp(dir)), MatrixPerspectiveFovLH(fov, 1.0f, zn, range));
                return 1;
            }
            static const Vector3 kDirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            static const Vector3 kUps[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
            const Matrix4 proj = MatrixPerspectiveFovLH(92.0f * kPi / 180.0f, 1.0f, zn, range);
            for (int face = 0; face < 6; ++face)
                out[face] = MatrixMultiply(MatrixLookToLH(position, kDirs[face], kUps[face]), proj);
            return 6;
        }
    }

    // Gives the shadow-casting lights among the binned cluster lights (nearest
    // first) their atlas tiles. Tile size follows the light's on-screen size;
    // a kept tile only shrinks once it is 4x too big, so it doesn't flip
    // between two sizes while the camera moves.
    static void AssignLocalShadows(FrameSetup& out, const std::vector<const Light*>& sources, const Vector3& eye, float yScale)
    {
        KES_ZONE_FN(RENDER);
        ShadowAtlasCache& cache = out.shadowCache;
        const ShadowConfig& config = out.shadows;
        SceneCBData& cb = out.scene;
        ++cache.frame;
        cache.frameEntries.clear();
        if (config.atlasSize != cache.atlasSize || config.maxTile != cache.maxTile)
        {
            cache.atlasSize = config.atlasSize;
            cache.maxTile = config.maxTile;
            const int cells = std::max(config.atlasSize / kMinShadowTile, 0);
            cache.cells.assign(static_cast<size_t>(cells * cells), 0);
            cache.entries.clear();
        }
        cache.entries.erase(std::remove_if(cache.entries.begin(), cache.entries.end(),
                                [](const ShadowAtlasCache::Entry& e) { return e.tileCount == 0; }),
            cache.entries.end());
        if (config.atlasSize < kMinShadowTile || config.cascadeCount == 0)
            return;

        struct Want
        {
            size_t gpuIndex;
            const Light* light;
            uint64_t key;
            int entry;
            int size;
            int count;
            Vector3 center;
            float radius;
        };
        std::vector<Want> wants;
        for (size_t i = 0; i < sources.size(); ++i)
        {
            const Light* light = sources[i];
            if (!light || !light->castShadows)
                continue;
            Want want = {};
            want.gpuIndex = i;
            want.light = light;
            want.key = light->id ? light->id : (reinterpret_cast<uintptr_t>(light) | (1ull << 63));
            want.count = light->type == LightType::Point ? 6 : 1;
            LocalLightBounds(*light, want.center, want.radius);
            const float dist = Vector3Length(want.center - eye);
            const float tanAngle = dist <= want.radius * 1.001f ? 1e3f : want.radius / std::sqrt(dist * dist - want.radius * want.radius);
            const float diameter = tanAngle * yScale * kShadowReferenceHeight;
            want.size = kMinShadowTile;
            while (want.size < config.maxTile && static_cast<float>(want.size) < diameter)
                want.size *= 2;
            if (want.count == 6 && want.size > kMinShadowTile)
                want.size /= 2;
            want.entry = -1;
            for (int e = 0; e < static_cast<int>(cache.entries.size()); ++e)
            {
                if (cache.entries[static_cast<size_t>(e)].lightKey == want.key)
                {
                    want.entry = e;
                    cache.entries[static_cast<size_t>(e)].lastUsedFrame = cache.frame;
                    break;
                }
            }
            wants.push_back(want);
        }

        // Nearest lights first up to the tile limit, then shrink the largest
        // tiles (farthest first) until all of them fit the atlas.
        int tileTotal = 0;
        size_t kept = 0;
        for (; kept < wants.size() && tileTotal + wants[kept].count <= kMaxShadowTiles; ++kept)
            tileTotal += wants[kept].count;
        wants.resize(kept);
        const int64_t atlasArea = static_cast<int64_t>(config.atlasSize) * config.atlasSize;
        bool pressure = false;
        for (;;)
        {
            int64_t area = 0;
            for (const Want& want : wants)
                area += static_cast<int64_t>(want.size) * want.size * want.count;
            if (area <= atlasArea)
                break;
            pressure = true;
            int pick = -1;
            for (int i = static_cast<int>(wants.size()) - 1; i >= 0; --i)
            {
                if (wants[static_cast<size_t>(i)].size > kMinShadowTile && (pick < 0 || wants[static_cast<size_t>(i)].size > wants[static_cast<size_t>(pick)].size))
                    pick = i;
            }
            if (pick < 0)
                break;
            wants[static_cast<size_t>(pick)].size /= 2;
        }

        // Largest tiles first: aligned power-of-two blocks then pack without gaps.
        std::vector<size_t> order(wants.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return wants[a].size > wants[b].size; });
        std::vector<bool> placed(wants.size(), false);
        for (size_t index : order)
        {
            Want& want = wants[index];
            int xs[6], ys[6];
            if (want.entry >= 0)
            {
                ShadowAtlasCache::Entry& entry = cache.entries[static_cast<size_t>(want.entry)];
                const bool lenient = !pressure && want.size < entry.tileSize && want.size * 4 > entry.tileSize;
                const bool keep = entry.tileCount == want.count && (want.size == entry.tileSize || lenient);
                if (!keep)
                {
                    // New tiles first, so a full atlas keeps the old ones.
                    if (AllocateTiles(cache, want.size, want.count, xs, ys))
                    {
                        FreeEntryTiles(cache, entry);
                        std::copy(xs, xs + want.count, entry.x);
                        std::copy(ys, ys + want.count, entry.y);
                        entry.tileSize = want.size;
                        entry.tileCount = want.count;
                    }
                    else if (entry.tileCount != want.count)
                    {
                        FreeEntryTiles(cache, entry);
                        continue;
                    }
                }
            }
            else
            {
                bool ok = false;
                int size = want.size;
                while (size >= kMinShadowTile && !(ok = AllocateTiles(cache, size, want.count, xs, ys)))
                    size /= 2;
                if (!ok)
                    continue;
                ShadowAtlasCache::Entry entry;
                entry.lightKey = want.key;
                entry.tileSize = size;
                entry.tileCount = want.count;
                std::copy(xs, xs + want.count, entry.x);
                std::copy(ys, ys + want.count, entry.y);
                entry.lastUsedFrame = cache.frame;
                cache.entries.push_back(entry);
                want.entry = static_cast<int>(cache.entries.size()) - 1;
            }
            placed[index] = true;
        }

        const float atlas = static_cast<float>(config.atlasSize);
        int tileIndex = 0;
        for (size_t index = 0; index < wants.size(); ++index)
        {
            if (!placed[index])
                continue;
            const Want& want = wants[index];
            ShadowAtlasCache::Entry& entry = cache.entries[static_cast<size_t>(want.entry)];
            const uint64_t lightState = LightShadowState(*want.light);
            if (lightState != entry.lightState)
            {
                entry.lightState = lightState;
                entry.contentValid = false;
            }
            Matrix4 matrices[6];
            LocalShadowMatrices(*want.light, matrices);
            entry.origin = want.light->transform.position;
            entry.range = std::max(want.light->range, 0.1f);
            entry.center = want.center;
            entry.radius = want.radius;
            for (int t = 0; t < entry.tileCount; ++t)
            {
                entry.viewProj[t] = matrices[t];
                CopyMatrix(matrices[t], cb.localShadowViewProj[tileIndex + t]);
                float* rect = cb.localShadowRects[tileIndex + t];
                rect[0] = rect[1] = static_cast<float>(entry.tileSize) / atlas;
                rect[2] = static_cast<float>(entry.x[t]) / atlas;
                rect[3] = static_cast<float>(entry.y[t]) / atlas;
            }
            out.clusterLights[want.gpuIndex].params[2] = static_cast<float>(tileIndex);
            cache.frameEntries.push_back(want.entry);
            tileIndex += entry.tileCount;
        }
    }

    void UpdateLocalShadows(FrameSetup& out, const std::vector<ShadowCaster>& casters)
    {
        KES_ZONE_FN(RENDER);
        ShadowAtlasCache& cache = out.shadowCache;

        // Spheres where shadows may have changed: casters that moved (old and
        // new place), appeared or went away.
        struct Sphere
        {
            Vector3 center;
            float radius;
        };
        std::vector<Sphere> changed;
        for (const ShadowCaster& caster : casters)
        {
            if (caster.key & ShadowCaster::kTransient)
            {
                changed.push_back({ caster.center, caster.radius });
                continue;
            }
            auto [it, inserted] = cache.casters.try_emplace(caster.key);
            ShadowAtlasCache::CasterRecord& record = it->second;
            if (record.seenFrame == cache.frame && !inserted)
                continue; // another section of the same object
            if (inserted)
                changed.push_back({ caster.center, caster.radius });
            else if (record.state != caster.state)
            {
                changed.push_back({ record.center, record.radius });
                changed.push_back({ caster.center, caster.radius });
            }
            record = { caster.state, caster.center, caster.radius, cache.frame };
        }
        for (auto it = cache.casters.begin(); it != cache.casters.end();)
        {
            if (it->second.seenFrame != cache.frame)
            {
                changed.push_back({ it->second.center, it->second.radius });
                it = cache.casters.erase(it);
            }
            else
                ++it;
        }

        for (ShadowAtlasCache::Entry& entry : cache.entries)
        {
            if (!entry.contentValid || entry.stale)
                continue;
            for (const Sphere& sphere : changed)
            {
                if (Vector3LengthSq(sphere.center - entry.center) <= (sphere.radius + entry.radius) * (sphere.radius + entry.radius))
                {
                    entry.stale = true;
                    entry.staleSince = cache.frame;
                    break;
                }
            }
        }

        const auto emit = [&](ShadowAtlasCache::Entry& entry)
        {
            for (int t = 0; t < entry.tileCount; ++t)
            {
                ShadowView view;
                view.viewProj = entry.viewProj[t];
                view.x = entry.x[t];
                view.y = entry.y[t];
                view.size = entry.tileSize;
                view.origin = entry.origin;
                view.radius = entry.range;
                out.shadowViews.push_back(view);
            }
            entry.contentValid = true;
            entry.stale = false;
        };
        // New tiles and changed lights now; caster refreshes within the budget, oldest first.
        std::vector<ShadowAtlasCache::Entry*> refresh;
        for (int index : cache.frameEntries)
        {
            ShadowAtlasCache::Entry& entry = cache.entries[static_cast<size_t>(index)];
            if (!entry.contentValid)
                emit(entry);
            else if (entry.stale)
                refresh.push_back(&entry);
        }
        std::stable_sort(refresh.begin(), refresh.end(),
            [](const ShadowAtlasCache::Entry* a, const ShadowAtlasCache::Entry* b) { return a->staleSince < b->staleSince; });
        int budget = kShadowRefreshBudget;
        for (ShadowAtlasCache::Entry* entry : refresh)
        {
            if (budget <= 0)
                break;
            budget -= entry->tileCount;
            emit(*entry);
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
        const float environmentParams[4] = { cb.environmentParams[0], cb.environmentParams[1], cb.environmentParams[2], cb.environmentParams[3] };
        cb = {};
        std::memcpy(cb.ambientSH, sh, sizeof(sh));
        std::memcpy(cb.environmentParams, environmentParams, sizeof(environmentParams));
        static const auto startTime = std::chrono::steady_clock::now();
        cb.environmentParams[2] = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime).count();

        CameraViewData cameraData = {};
        CameraBuildMatrices(camera, aspect, cameraData);
        std::memcpy(out.viewProj.m, cameraData.viewProjection, sizeof(out.viewProj.m));
        CopyMatrix(out.viewProj, cb.viewProj);
        if (!InvertMatrix(cb.viewProj, cb.invViewProj))
            std::memset(cb.invViewProj, 0, sizeof(cb.invViewProj));
        cb.depthParams[0] = kCameraNearPlane;
        cb.depthParams[1] = kCameraFarPlane;
        cb.contactShadowParams[2] = -1.0f;
        out.frustum = ExtractFrustum(out.viewProj);

        const Vector3 eye = CameraGetPosition(camera);
        const Vector3 forward = CameraGetForward(camera);
        cb.cameraPosition[0] = eye.x;
        cb.cameraPosition[1] = eye.y;
        cb.cameraPosition[2] = eye.z;
        cb.cameraForward[0] = forward.x;
        cb.cameraForward[1] = forward.y;
        cb.cameraForward[2] = forward.z;

        // Directional lights go into the scene constants in scene order, visible
        // local lights into the clusters nearest first (shadow tiles in the same
        // order), then the particle lights.
        const std::vector<ScenePointLight>& pointLights = scene.GetParticles().lights;
        std::vector<LightCandidate> candidates;
        candidates.reserve(scene.GetLights().size());
        for (const Light& light : scene.GetLights())
        {
            if (!light.active || light.intensity <= 0.0f || Luminance(light.color) <= 0.0f)
                continue;
            if (light.type == LightType::Directional)
            {
                candidates.push_back({ &light, -1e30f });
                continue;
            }
            Vector3 center;
            float radius = 0.0f;
            LocalLightBounds(light, center, radius);
            if (!SphereInFrustum(out.frustum, center, radius))
                continue;
            candidates.push_back({ &light, Vector3Length(center - eye) - radius, center, radius });
        }
        std::stable_sort(candidates.begin(), candidates.end(),
            [](const LightCandidate& a, const LightCandidate& b)
            { return a.score < b.score; });

        std::vector<ClusterCandidate> clustered;
        clustered.reserve(candidates.size() + pointLights.size());
        int lightCount = 0;
        bool sunAssigned = false;
        for (const LightCandidate& candidate : candidates)
        {
            const Light& light = *candidate.light;
            GPULight* slot = nullptr;
            if (light.type == LightType::Directional)
            {
                if (lightCount >= kMaxDirectionalLights)
                    continue;
                slot = &cb.lights[lightCount++];
            }
            else
            {
                if (clustered.size() >= static_cast<size_t>(kMaxClusterLights))
                    break;
                clustered.push_back({ {}, candidate.center, candidate.radius, &light });
                slot = &clustered.back().gpu;
            }
            GPULight& gpu = *slot;
            PackLight(light, gpu);
            if (!light.castShadows || out.shadows.cascadeCount == 0 || light.type != LightType::Directional || sunAssigned)
                continue;
            sunAssigned = true;
            gpu.params[2] = 1.0f;

            const Vector3 lightDir = LightDirection(light);
            const float nearPlane = kCameraNearPlane;
            const float farPlane = std::clamp(options.shadowDistance, 5.0f, kCameraFarPlane);
            const int count = out.shadows.cascadeCount;
            float splits[kMaxCascades + 1] = { nearPlane };
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
        }
        cb.lightCounts[0] = static_cast<float>(lightCount);
        // Contact shadows follow the shadowed sun, else the first directional light.
        if (lightCount > 0)
            cb.contactShadowParams[2] = 0.0f;
        for (int i = 0; i < lightCount; ++i)
        {
            if (cb.lights[i].params[2] > 0.5f)
            {
                cb.contactShadowParams[2] = static_cast<float>(i);
                break;
            }
        }

        for (const ScenePointLight& light : pointLights)
        {
            ClusterCandidate c;
            c.center = Vector3(light.position[0], light.position[1], light.position[2]);
            c.radius = light.radius;
            c.gpu.position[0] = c.center.x;
            c.gpu.position[1] = c.center.y;
            c.gpu.position[2] = c.center.z;
            c.gpu.position[3] = std::max(light.radius, 0.01f);
            for (int i = 0; i < 3; ++i)
                c.gpu.color[i] = light.color[i] * kPi; // same radiance scale as regular lights
            c.gpu.params[0] = static_cast<float>(LightType::Point);
            c.gpu.params[2] = -1.0f;
            clustered.push_back(c);
        }
        std::vector<const Light*> clusterSources;
        BuildClusters(out, cameraData, clustered, clusterSources);
        AssignLocalShadows(out, clusterSources, eye, cameraData.projection[5]);
        if (out.shadows.cascadeCount > 0)
        {
            cb.shadowParams[1] = 1.0f / static_cast<float>(out.shadows.cascadeResolution);
            cb.shadowParams[2] = 1.0f / static_cast<float>(std::max(out.shadows.atlasSize, 1));
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
            case 0:  return Normalize({ 1.0f, -v, -u });
            case 1:  return Normalize({ -1.0f, -v, u });
            case 2:  return Normalize({ u, 1.0f, v });
            case 3:  return Normalize({ u, -1.0f, -v });
            case 4:  return Normalize({ u, -v, 1.0f });
            default: return Normalize({ -u, -v, -1.0f });
            }
        }

        Vector3 ImportanceSampleGGX(float xi0, float xi1, float roughness, const Vector3& n)
        {
            const float a = roughness * roughness;
            const float phi = 2.0f * kPi * xi0;
            const float cosTheta = std::sqrt((1.0f - xi1) / (1.0f + (a * a - 1.0f) * xi1));
            const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const Vector3 h = { sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta };
            const Vector3 up = std::fabs(n.z) < 0.999f ? Vector3{ 0, 0, 1 } : Vector3{ 1, 0, 0 };
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
        return hash | 1u; // never 0 ("not built")
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
                const Vector3 n = Normalize({ std::sqrt(std::max(0.0f, 1.0f - y * y)), y, 0.0f });
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
                table[k] = { sum[0], sum[1], sum[2] };
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
                        1.092548 * d.x * d.z, 0.546274 * (d.x * d.x - d.y * d.y)
                    };
                    for (int i = 0; i < 9; ++i)
                        for (int c = 0; c < 3; ++c)
                            coefficients[i][c] += radiance[c] * basis[i] * weight;
                    totalWeight += weight;
                }
            }
        }
        const double normalize = 4.0 * kPi / totalWeight;
        const double band[9] = { 1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25 }; // A_l / pi
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
                        Vector3 n = Normalize({ sum[0], sum[1], sum[2] });
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
            return Vector3{ p[0], p[1], p[2] };
        };
        Vector3 minV = position(0), maxV = minV;
        for (size_t i = 1; i < count; ++i)
        {
            const Vector3 p = position(i);
            minV = { std::min(minV.x, p.x), std::min(minV.y, p.y), std::min(minV.z, p.z) };
            maxV = { std::max(maxV.x, p.x), std::max(maxV.y, p.y), std::max(maxV.z, p.z) };
        }
        outCenter = (minV + maxV) * 0.5f;
        float radiusSq = 0.0f;
        for (size_t i = 0; i < count; ++i)
            radiusSq = std::max(radiusSq, Vector3LengthSq(position(i) - outCenter));
        outRadius = std::sqrt(radiusSq);
    }

    void TransformSphere(const Matrix4& world, const Vector3& scale, const Vector3& localCenter, float localRadius, Vector3& outCenter, float& outRadius)
    {
        outCenter = TransformPoint(world, localCenter);
        outRadius = localRadius * std::max({ std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z) });
    }
    int ComputeBloomLevels(int renderWidth, int renderHeight, int outSizes[kBloomMaxLevels][2])
    {
        int width = renderWidth / 2;
        int height = renderHeight / 2;
        int count = 0;
        while (count < kBloomMaxLevels && width >= 2 && height >= 2)
        {
            outSizes[count][0] = width;
            outSizes[count][1] = height;
            ++count;
            width /= 2;
            height /= 2;
        }
        return count;
    }

    bool InvertMatrix(const float m[16], float out[16])
    {
        float inv[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
        const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (std::fabs(det) < 1e-20f)
            return false;
        const float invDet = 1.0f / det;
        for (int i = 0; i < 16; ++i)
            out[i] = inv[i] * invDet;
        return true;
    }

    void SetScreenSpaceLighting(SceneCBData& cb, const PostProcessSettings& post, const GraphicsOptions& options,
        int renderWidth, int renderHeight, const ScreenSpaceAvailability& availability)
    {
        const bool available = availability.lighting;
        const float* prevViewProj = availability.prevViewProj;
        cb.screenParams[0] = static_cast<float>(renderWidth);
        cb.screenParams[1] = static_cast<float>(renderHeight);
        cb.screenParams[2] = renderWidth > 0 ? 1.0f / static_cast<float>(renderWidth) : 0.0f;
        cb.screenParams[3] = renderHeight > 0 ? 1.0f / static_cast<float>(renderHeight) : 0.0f;
        const bool ao = available && options.ambientOcclusion && post.ambientOcclusionIntensity > 0.0f &&
            post.ambientOcclusionRadius > 0.0f;
        const bool contact = available && options.contactShadows && post.contactShadowLength > 0.0f &&
            cb.contactShadowParams[2] >= 0.0f;
        cb.ssaoParams[0] = std::clamp(post.ambientOcclusionRadius, 0.01f, 10.0f);
        cb.ssaoParams[1] = std::clamp(post.ambientOcclusionIntensity, 0.0f, 8.0f);
        cb.ssaoParams[2] = ao ? 1.0f : 0.0f;
        cb.ssaoParams[3] = contact ? 1.0f : 0.0f;
        cb.contactShadowParams[0] = std::clamp(post.contactShadowLength, 0.0f, 5.0f);
        cb.contactShadowParams[1] = 0.15f;

        const bool reflections = availability.reflections && WantsScreenReflections(post, options) && prevViewProj;
        if (prevViewProj)
            std::memcpy(cb.prevViewProj, prevViewProj, sizeof(cb.prevViewProj));
        cb.ssrParams[0] = std::clamp(post.reflectionMaxDistance, 1.0f, 200.0f);
        cb.ssrParams[1] = std::clamp(post.reflectionMaxRoughness, 0.0f, 1.0f);
        cb.ssrParams[2] = reflections ? 1.0f : 0.0f;
        cb.ssrParams[3] = std::clamp(post.reflectionIntensity, 0.0f, 1.0f);

        const bool dof = availability.depthOfField && WantsDepthOfField(post, options);
        cb.dofParams[0] = std::max(post.depthOfFieldFocusDistance, 0.0f);
        cb.dofParams[1] = std::max(post.depthOfFieldFocusRange, 0.01f);
        cb.dofParams[2] = std::clamp(post.depthOfFieldMaxBlur, 0.0f, 5.0f) * 0.01f * static_cast<float>(renderHeight);
        cb.dofParams[3] = dof && cb.dofParams[2] >= 1.0f ? 1.0f : 0.0f;

        // Jitter (xy) is written by ApplyTemporalJitter.
        cb.taaParams[2] = availability.temporalAA ? 1.0f : 0.0f;
        cb.taaParams[3] = availability.temporalAA && availability.temporalHistory && prevViewProj ? 1.0f : 0.0f;

        const bool motionBlur = availability.motionBlur && WantsMotionBlur(post, options) && prevViewProj;
        cb.motionBlurParams[0] = std::clamp(post.motionBlurIntensity, 0.0f, 2.0f);
        cb.motionBlurParams[1] = 0.05f;
        cb.motionBlurParams[2] = motionBlur ? 1.0f : 0.0f;
    }

    void ApplyTemporalJitter(FrameSetup& out, uint32_t frameIndex, int renderWidth, int renderHeight)
    {
        const auto halton = [](uint32_t index, uint32_t base)
        {
            float f = 1.0f;
            float result = 0.0f;
            while (index > 0)
            {
                f /= static_cast<float>(base);
                result += f * static_cast<float>(index % base);
                index /= base;
            }
            return result;
        };
        const uint32_t phase = frameIndex % 8u + 1u;
        // Clip-space offset: clip.xy += jitter * clip.w.
        const float jx = (halton(phase, 2) - 0.5f) * 2.0f / static_cast<float>(std::max(renderWidth, 1));
        const float jy = (halton(phase, 3) - 0.5f) * 2.0f / static_cast<float>(std::max(renderHeight, 1));
        for (int row = 0; row < 4; ++row)
        {
            out.viewProj.m[row * 4 + 0] += jx * out.viewProj.m[row * 4 + 3];
            out.viewProj.m[row * 4 + 1] += jy * out.viewProj.m[row * 4 + 3];
        }
        SceneCBData& cb = out.scene;
        CopyMatrix(out.viewProj, cb.viewProj);
        if (!InvertMatrix(cb.viewProj, cb.invViewProj))
            std::memset(cb.invViewProj, 0, sizeof(cb.invViewProj));
        cb.taaParams[0] = 0.5f * jx;
        cb.taaParams[1] = -0.5f * jy;
    }

    bool WantsMotionBlur(const PostProcessSettings& post, const GraphicsOptions& options)
    {
        return options.motionBlur && post.motionBlurIntensity > 0.0f;
    }

    bool WantsDepthOfField(const PostProcessSettings& post, const GraphicsOptions& options)
    {
        return options.depthOfField && post.depthOfFieldMaxBlur > 0.0f;
    }

    bool WantsScreenReflections(const PostProcessSettings& post, const GraphicsOptions& options)
    {
        return options.screenSpaceReflections && post.reflectionIntensity > 0.0f && post.reflectionMaxRoughness > 0.0f;
    }

    bool WantsDepthPrepass(const SceneCBData& cb)
    {
        return cb.ssaoParams[2] > 0.5f || cb.ssaoParams[3] > 0.5f || cb.ssrParams[2] > 0.5f || cb.dofParams[3] > 0.5f ||
            cb.taaParams[2] > 0.5f || cb.motionBlurParams[2] > 0.5f;
    }

    void ComputeWhiteBalanceLms(float temperature, float tint, float out[3])
    {
        const float t1 = std::clamp(temperature, -1.0f, 1.0f) * (100.0f / 65.0f);
        const float t2 = std::clamp(tint, -1.0f, 1.0f) * (100.0f / 65.0f);
        if (t1 == 0.0f && t2 == 0.0f)
        {
            out[0] = out[1] = out[2] = 1.0f;
            return;
        }
        const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
        const float y = 2.87f * x - 3.0f * x * x - 0.27509507f + t2 * 0.05f;
        const float X = x / y;
        const float Z = (1.0f - x - y) / y;
        const float L = 0.7328f * X + 0.4296f - 0.1624f * Z;
        const float M = -0.7036f * X + 1.6975f + 0.0061f * Z;
        const float S = 0.0030f * X + 0.0136f + 0.9834f * Z;
        out[0] = 0.949237f / L;
        out[1] = 1.03542f / M;
        out[2] = 1.08728f / S;
    }

    TonemapCBData BuildTonemapConstants(const SceneState& state, const PostProcessSettings& post, bool autoExposure,
        int bloomLevels, int renderWidth, int renderHeight)
    {
        TonemapCBData out = {};
        out.params[0] = std::max(state.exposure, 0.0f) * std::exp2(std::clamp(post.exposureCompensation, -16.0f, 16.0f));
        out.params[1] = 1.0f / 255.0f;
        out.params[2] = autoExposure && post.autoExposure ? 1.0f : 0.0f;
        out.params[3] = bloomLevels > 0 ? std::clamp(post.bloomIntensity, 0.0f, 1.0f) : 0.0f;
        out.vignette[0] = std::clamp(post.vignetteIntensity, 0.0f, 1.0f);
        out.vignette[1] = std::clamp(post.vignetteSmoothness, 0.0f, 1.0f);
        out.vignette[2] = renderHeight > 0 ? static_cast<float>(renderWidth) / static_cast<float>(renderHeight) : 1.0f;
        out.vignette[3] = bloomLevels > 0 ? 1.0f / static_cast<float>(bloomLevels) : 0.0f;
        for (int i = 0; i < 3; ++i)
            out.vignetteColor[i] = std::max(post.vignetteColor[i], 0.0f);
        out.vignetteColor[3] = static_cast<float>(std::clamp(static_cast<int>(post.tonemapper), 0, static_cast<int>(Tonemapper::Count) - 1));

        ComputeWhiteBalanceLms(post.temperature, post.tint, out.whiteBalance);
        out.whiteBalance[3] = std::max(post.saturation, 0.0f);
        for (int i = 0; i < 3; ++i)
        {
            out.colorFilter[i] = std::max(post.colorFilter[i], 0.0f);
            out.lift[i] = std::clamp(post.lift[i], -1.0f, 1.0f);
            out.gamma[i] = 1.0f / std::max(post.gamma[i], 0.01f);
            out.gain[i] = std::max(post.gain[i], 0.0f);
        }
        out.colorFilter[3] = std::max(post.contrast, 0.0f);
        out.lift[3] = std::clamp(post.sharpen, 0.0f, 1.0f);
        out.gamma[3] = renderWidth > 0 ? 1.0f / static_cast<float>(renderWidth) : 0.0f;
        out.gain[3] = renderHeight > 0 ? 1.0f / static_cast<float>(renderHeight) : 0.0f;
        return out;
    }

    ExposureCBData BuildExposureConstants(const PostProcessSettings& post, float deltaTime, bool reset, int renderWidth, int renderHeight)
    {
        ExposureCBData out = {};
        out.params0[0] = kHistogramMinLog2;
        out.params0[1] = 1.0f / kHistogramLog2Range;
        out.params0[2] = static_cast<float>(renderWidth);
        out.params0[3] = static_cast<float>(renderHeight);
        out.params1[0] = std::clamp(deltaTime, 0.0f, 0.25f);
        out.params1[1] = std::max(post.adaptationSpeedUp, 0.0f);
        out.params1[2] = std::max(post.adaptationSpeedDown, 0.0f);
        out.params1[3] = reset ? 1.0f : 0.0f;
        out.params2[0] = std::min(post.autoExposureMin, post.autoExposureMax);
        out.params2[1] = std::max(post.autoExposureMin, post.autoExposureMax);
        return out;
    }

    void BuildParticleGeometry(const Scene& scene, const Camera& camera, ParticleGeometry& out)
    {
        KES_ZONE_FN(RENDER);
        out.vertices.clear();
        out.indices.clear();
        out.draws.clear();
        const SceneParticles& particles = scene.GetParticles();
        if (particles.sprites.empty() && particles.ribbons.empty())
            return;

        ParticleCameraBasis basis;
        basis.position = CameraGetPosition(camera);
        basis.right = CameraGetRight(camera);
        basis.up = CameraGetUp(camera);
        basis.forward = CameraGetForward(camera);

        thread_local std::vector<ParticleSpriteInstance> sprites;
        for (const SceneParticleSprites& batch : particles.sprites)
        {
            ParticleDraw draw;
            draw.material = batch.material;
            draw.blend = batch.settings.blend;
            draw.lit = batch.settings.lit;
            draw.subImages = batch.settings.columns * batch.settings.rows > 1;
            draw.sortOrder = batch.settings.sortOrder;
            draw.firstIndex = static_cast<uint32_t>(out.indices.size());
            draw.distanceSq = Vector3LengthSq(batch.center - basis.position);
            sprites = batch.sprites;
            BuildParticleSpriteGeometry(batch.settings, sprites, basis, out.vertices, out.indices);
            draw.indexCount = static_cast<uint32_t>(out.indices.size()) - draw.firstIndex;
            if (draw.indexCount > 0)
                out.draws.push_back(draw);
        }
        for (const SceneParticleRibbon& batch : particles.ribbons)
        {
            ParticleDraw draw;
            draw.material = batch.material;
            draw.blend = batch.settings.blend;
            draw.lit = batch.settings.lit;
            draw.ribbon = true;
            draw.sortOrder = batch.settings.sortOrder;
            draw.firstIndex = static_cast<uint32_t>(out.indices.size());
            draw.distanceSq = Vector3LengthSq(batch.center - basis.position);
            BuildParticleRibbonGeometry(batch.settings, batch.points, basis, out.vertices, out.indices);
            draw.indexCount = static_cast<uint32_t>(out.indices.size()) - draw.firstIndex;
            if (draw.indexCount > 0)
                out.draws.push_back(draw);
        }
        std::stable_sort(out.draws.begin(), out.draws.end(), [](const ParticleDraw& a, const ParticleDraw& b)
            { return ParticleBatchDrawsBefore(a.sortOrder, a.blend, a.distanceSq, b.sortOrder, b.blend, b.distanceSq); });
    }

    void ParticleDrawFlags(const ParticleDraw& draw, float outSkinInfo[4])
    {
        outSkinInfo[0] = draw.ribbon ? 2.0f : (draw.subImages ? 0.0f : 1.0f);
        outSkinInfo[1] = draw.lit ? 1.0f : 0.0f;
        outSkinInfo[2] = draw.blend == ParticleBlend::Alpha ? 0.0f : 1.0f;
        outSkinInfo[3] = draw.blend == ParticleBlend::Additive ? 1.0f : 0.0f;
    }
}

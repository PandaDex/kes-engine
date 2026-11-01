#include "camera.h"
#include <cmath>

namespace
{
    struct float3
    {
        float x, y, z;
    };

    struct float4x4
    {
        float m[4][4];
    };

    static constexpr float kCameraFovYRadians = 70.0f * 3.14159265f / 180.0f;

    static float4x4 Identity()
    {
        float4x4 r = {};
        r.m[0][0] = 1.0f;
        r.m[1][1] = 1.0f;
        r.m[2][2] = 1.0f;
        r.m[3][3] = 1.0f;
        return r;
    }

    static float4x4 Multiply(const float4x4& a, const float4x4& b)
    {
        float4x4 r = {};
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                r.m[row][col] =
                    a.m[row][0] * b.m[0][col] +
                    a.m[row][1] * b.m[1][col] +
                    a.m[row][2] * b.m[2][col] +
                    a.m[row][3] * b.m[3][col];
            }
        }
        return r;
    }

    static float4x4 PerspectiveFovLH(float fovY, float aspect, float zn, float zf)
    {
        float4x4 r = {};
        const float yScale = 1.0f / std::tan(fovY * 0.5f);
        const float xScale = yScale / aspect;

        r.m[0][0] = xScale;
        r.m[1][1] = yScale;
        r.m[2][2] = zf / (zf - zn);
        r.m[2][3] = 1.0f;
        r.m[3][2] = (-zn * zf) / (zf - zn);
        return r;
    }

    static void MatrixToArray(const float4x4& m, float out[16])
    {
        int i = 0;
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                out[i++] = m.m[row][col];
            }
        }
    }

    static float3 Normalize(const float3& v)
    {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (len <= 0.00001f)
            return { 0.0f, 0.0f, 1.0f };

        return { v.x / len, v.y / len, v.z / len };
    }

    static float3 Cross(const float3& a, const float3& b)
    {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    static float Dot(const float3& a, const float3& b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static float4x4 LookToLH(const float3& eye, const float3& forward, const float3& upRef)
    {
        const float3 zaxis = Normalize(forward);
        const float3 xaxis = Normalize(Cross(upRef, zaxis));
        const float3 yaxis = Cross(zaxis, xaxis);

        float4x4 r = {};
        r.m[0][0] = xaxis.x; r.m[0][1] = yaxis.x; r.m[0][2] = zaxis.x; r.m[0][3] = 0.0f;
        r.m[1][0] = xaxis.y; r.m[1][1] = yaxis.y; r.m[1][2] = zaxis.y; r.m[1][3] = 0.0f;
        r.m[2][0] = xaxis.z; r.m[2][1] = yaxis.z; r.m[2][2] = zaxis.z; r.m[2][3] = 0.0f;
        r.m[3][0] = -Dot(xaxis, eye);
        r.m[3][1] = -Dot(yaxis, eye);
        r.m[3][2] = -Dot(zaxis, eye);
        r.m[3][3] = 1.0f;
        return r;
    }

    static Vector3 MakeCameraForward(const Camera& camera)
    {
        const float cp = std::cos(camera.pitch);
        const float sp = std::sin(camera.pitch);
        const float cy = std::cos(camera.yaw);
        const float sy = std::sin(camera.yaw);
        return Vector3Normalize({ sy * cp, -sp, cy * cp });
    }

    static Vector3 MakeCameraRight(const Camera& camera)
    {
        const Vector3 forward = MakeCameraForward(camera);
        Vector3 right = Vector3Normalize(Vector3Cross({ 0.0f, 1.0f, 0.0f }, forward));
        if (Vector3LengthSq(right) <= 0.00001f)
            right = { 1.0f, 0.0f, 0.0f };
        return right;
    }

    static Vector3 MakeCameraUp(const Camera& camera)
    {
        return Vector3Normalize(Vector3Cross(CameraGetForward(camera), CameraGetRight(camera)));
    }
}

void CameraBuildMatrices(const Camera& camera, float aspect, CameraViewData& outData)
{
    const float3 eye = { camera.x, camera.y, camera.z };
    const Vector3 cameraForward = CameraGetForward(camera);
    const float3 forward = { cameraForward.x, cameraForward.y, cameraForward.z };
    const float3 up = { 0.0f, 1.0f, 0.0f };

    const float4x4 view = LookToLH(eye, forward, up);
    const float4x4 proj = PerspectiveFovLH(kCameraFovYRadians, aspect, 0.1f, 100.0f);
    const float4x4 vp = Multiply(view, proj);

    MatrixToArray(view, outData.view);
    MatrixToArray(proj, outData.projection);
    MatrixToArray(vp, outData.viewProjection);
}

Vector3 CameraGetPosition(const Camera& camera)
{
    return { camera.x, camera.y, camera.z };
}

Vector3 CameraGetForward(const Camera& camera)
{
    return MakeCameraForward(camera);
}

Vector3 CameraGetRight(const Camera& camera)
{
    return MakeCameraRight(camera);
}

Vector3 CameraGetUp(const Camera& camera)
{
    return MakeCameraUp(camera);
}

Ray CameraBuildRay(const Camera& camera)
{
    Ray ray = {};
    ray.origin = CameraGetPosition(camera);
    ray.direction = CameraGetForward(camera);
    return ray;
}

Ray CameraBuildViewportRay(const Camera& camera, float aspect, const Vector2& viewportUv)
{
    const float safeAspect = aspect > 0.00001f ? aspect : 1.0f;
    const float tanHalfFov = std::tan(kCameraFovYRadians * 0.5f);
    const float ndcX = viewportUv.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - viewportUv.y * 2.0f;

    Ray ray = {};
    ray.origin = CameraGetPosition(camera);
    ray.direction = Vector3Normalize(
        CameraGetForward(camera) +
        CameraGetRight(camera) * (ndcX * tanHalfFov * safeAspect) +
        CameraGetUp(camera) * (ndcY * tanHalfFov));
    return ray;
}

bool CameraRaycast(const Camera& camera, const CollisionWorld& world, RaycastHit& outHit, const RaycastQuery& query)
{
    return world.Raycast(CameraBuildRay(camera), outHit, query);
}

bool CameraRaycastViewport(const Camera& camera, float aspect, const Vector2& viewportUv, const CollisionWorld& world, RaycastHit& outHit, const RaycastQuery& query)
{
    return world.Raycast(CameraBuildViewportRay(camera, aspect, viewportUv), outHit, query);
}

#pragma once
#include "collision_world.h"

struct Camera
{
    float x = 0.0f;
    float y = 0.0f;
    float z = -6.0f;

    float pitch = 0.0f;
    float yaw = 0.0f;
};

// Projection shared by the renderers, shadows and picking.
inline constexpr float kCameraFovYRadians = 70.0f * 3.14159265f / 180.0f;
inline constexpr float kCameraNearPlane = 0.1f;
inline constexpr float kCameraFarPlane = 500.0f;

struct CameraViewData
{
    float view[16];
    float projection[16];
    float viewProjection[16];
};

void CameraBuildMatrices(
    const Camera& camera,
    float aspect,
    CameraViewData& outData);

Vector3 CameraGetPosition(const Camera& camera);
Vector3 CameraGetForward(const Camera& camera);
Vector3 CameraGetRight(const Camera& camera);
Vector3 CameraGetUp(const Camera& camera);

Ray CameraBuildRay(const Camera& camera);
Ray CameraBuildViewportRay(const Camera& camera, float aspect, const Vector2& viewportUv);

bool CameraRaycast(const Camera& camera, const CollisionWorld& world, RaycastHit& outHit, const RaycastQuery& query = {});
bool CameraRaycastViewport(const Camera& camera, float aspect, const Vector2& viewportUv, const CollisionWorld& world, RaycastHit& outHit, const RaycastQuery& query = {});

#pragma once
#include <vector>
#include "entity.h"

// Solid map entity that renders its brushes and (optionally) collides with them.
// The map editor's equivalent of Source's func_brush: use it for geometry that
// game code wants to move, hide or toggle.
//
// Properties:
//   Solid    true/false  collide with the brushes (default true)
//   Visible  true/false  render the brushes (default true)
//   Dynamic  true/false  simulate it as one rigid body (default false)
//   Density, Friction, Restitution  for dynamic brushes
class BrushEntity : public Entity
{
public:
    void Start() override;
    void Update(float dt) override;
    void Teleport(const Vector3& newPosition) override;
    bool ApplyProperty(const std::string& name, const std::string& value) override;
    void ApplyMapGeometry(const MapEntityGeometry& geometry) override;
    bool OwnsCollider(ColliderHandle handle) const override;

    void SetVisible(bool visible);
    void SetSolid(bool solid);
    bool IsVisible() const { return visible; }
    bool IsSolid() const { return solid; }

private:
    void SyncTransforms();
    void OnDestroy() override;

    MapEntityGeometry geometry;
    std::vector<uint32_t> objectIds;
    std::vector<ColliderHandle> colliders;
    bool visible = true;
    bool solid = true;
    bool started = false;
    RigidBodyDesc body = MakeStaticDesc();

    static RigidBodyDesc MakeStaticDesc()
    {
        RigidBodyDesc desc;
        desc.motion = ColliderMotion::Static;
        return desc;
    }
    bool IsDynamic() const { return body.motion == ColliderMotion::Dynamic; }
};

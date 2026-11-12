#include "ent_brush.h"
#include <algorithm>
#include "collision_world.h"
#include "entity_property_utils.h"
#include "game_context.h"
#include "scene.h"

void BrushEntity::ApplyMapGeometry(const MapEntityGeometry& mapGeometry)
{
    geometry = mapGeometry;
}

void BrushEntity::Start()
{
    started = true;
    if (!game)
        return;

    if (game->scene)
    {
        for (const MapEntityGeometry::Mesh& mesh : geometry.meshes)
        {
            SceneObject object = {};
            object.mesh = mesh.mesh;
            object.material = mesh.material;
            object.transform.position = Position;
            object.transform.scale = { 1.0f, 1.0f, 1.0f };
            object.active = visible;
            objectIds.push_back(game->scene->CreateObject(object));
        }
    }

    if (game->collision && IsDynamic() && !geometry.hulls.empty())
    {
        // Brush geometry already includes the rotation from the editor.
        Rotation = {};
        body.position = Position;
        const ColliderHandle handle = game->collision->AddRigidHulls(geometry.hulls, body);
        game->collision->SetActive(handle, solid);
        colliders.push_back(handle);
    }
    else if (game->collision)
    {
        for (const MapEntityGeometry::Hull& hull : geometry.hulls)
        {
            const ColliderHandle handle = game->collision->AddConvex(Position, hull.planes, hull.boundsMin, hull.boundsMax);
            game->collision->SetActive(handle, solid);
            colliders.push_back(handle);
        }
    }
}

void BrushEntity::Update(float dt)
{
    (void)dt;
    if (!IsDynamic() || colliders.empty() || !game || !game->collision || !game->scene)
        return;

    if (game->collision->GetBodyPose(colliders.front(), game->fixedStepAlpha, Position, Rotation))
    {
        for (uint32_t id : objectIds)
        {
            if (SceneObject* object = game->scene->FindObject(id))
            {
                object->transform.position = Position;
                object->transform.rotation = Rotation;
            }
        }
    }
}

bool BrushEntity::OwnsCollider(ColliderHandle handle) const
{
    return std::find(colliders.begin(), colliders.end(), handle) != colliders.end();
}

void BrushEntity::Teleport(const Vector3& newPosition)
{
    Entity::Teleport(newPosition);
    SyncTransforms();
}

bool BrushEntity::ApplyProperty(const std::string& name, const std::string& value)
{
    if (name == "Solid")
    {
        SetSolid(entityprops::ParseBool(value, solid));
        return true;
    }
    if (name == "Visible")
    {
        SetVisible(entityprops::ParseBool(value, visible));
        return true;
    }
    if (ApplyRigidBodyProperty(body, name, value))
        return true;
    return Entity::ApplyProperty(name, value);
}

void BrushEntity::SetVisible(bool value)
{
    visible = value;
    if (!started || !game || !game->scene)
        return;
    for (uint32_t id : objectIds)
    {
        if (SceneObject* object = game->scene->FindObject(id))
            object->active = visible;
    }
}

void BrushEntity::SetSolid(bool value)
{
    solid = value;
    if (!started || !game || !game->collision)
        return;
    for (ColliderHandle handle : colliders)
        game->collision->SetActive(handle, solid);
}

void BrushEntity::SyncTransforms()
{
    if (!started || !game)
        return;
    if (game->scene)
    {
        for (uint32_t id : objectIds)
        {
            if (SceneObject* object = game->scene->FindObject(id))
                object->transform.position = Position;
        }
    }
    if (game->collision)
    {
        for (ColliderHandle handle : colliders)
            game->collision->SetPosition(handle, Position);
    }
}

void BrushEntity::OnDestroy()
{
    if (!game)
        return;
    if (game->scene)
    {
        for (uint32_t id : objectIds)
            game->scene->RemoveObject(id);
    }
    if (game->collision)
    {
        for (ColliderHandle handle : colliders)
            game->collision->Remove(handle);
    }
    objectIds.clear();
    colliders.clear();
}

const MapScriptPorts& BrushEntity::GetMapScriptPorts() const
{
    static const MapScriptPorts ports = {
        { { "Enable" }, { "Disable" }, { "Toggle" }, { "Show" }, { "Hide" },
          { "EnableCollision" }, { "DisableCollision" } },
        {},
    };
    return ports;
}

bool BrushEntity::AcceptInput(const std::string& input, const MapScriptCall& call)
{
    (void)call;
    if (input == "Enable" || input == "Disable" || input == "Toggle")
    {
        const bool on = input == "Enable" || (input == "Toggle" && !(visible || solid));
        SetVisible(on);
        SetSolid(on);
        return true;
    }
    if (input == "Show" || input == "Hide")
    {
        SetVisible(input == "Show");
        return true;
    }
    if (input == "EnableCollision" || input == "DisableCollision")
    {
        SetSolid(input == "EnableCollision");
        return true;
    }
    return false;
}

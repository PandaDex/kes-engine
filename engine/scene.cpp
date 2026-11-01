#include "scene.h"

uint32_t Scene::CreateObject(const SceneObject& object)
{
    SceneObject copy = object;
    copy.id = nextObjectId++;
    objects.push_back(copy);
    return copy.id;
}

bool Scene::RemoveObject(uint32_t id)
{
    for (size_t i = 0; i < objects.size(); ++i)
    {
        if (objects[i].id == id)
        {
            objects.erase(objects.begin() + (long long)i);
            return true;
        }
    }
    return false;
}

SceneObject* Scene::FindObject(uint32_t id)
{
    for (SceneObject& object : objects)
    {
        if (object.id == id)
            return &object;
    }
    return nullptr;
}

const SceneObject* Scene::FindObject(uint32_t id) const
{
    for (const SceneObject& object : objects)
    {
        if (object.id == id)
            return &object;
    }
    return nullptr;
}

uint32_t Scene::CreateLight(const Light& light)
{
    Light copy = light;
    copy.id = nextLightId++;
    lights.push_back(copy);
    return copy.id;
}

bool Scene::RemoveLight(uint32_t id)
{
    for (size_t i = 0; i < lights.size(); ++i)
    {
        if (lights[i].id == id)
        {
            lights.erase(lights.begin() + (long long)i);
            return true;
        }
    }
    return false;
}

Light* Scene::FindLight(uint32_t id)
{
    for (Light& light : lights)
    {
        if (light.id == id)
            return &light;
    }
    return nullptr;
}

const Light* Scene::FindLight(uint32_t id) const
{
    for (const Light& light : lights)
    {
        if (light.id == id)
            return &light;
    }
    return nullptr;
}

void Scene::Clear()
{
    objects.clear();
    lights.clear();
    sky = {};
    fog = {};
    nextObjectId = 1;
    nextLightId = 1;
}

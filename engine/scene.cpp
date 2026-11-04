#include "scene.h"

uint32_t Scene::CreateObject(const SceneObject& object)
{
    const uint32_t id = nextObjectId++;
    objects.push_back(object);
    objects.back().id = id;
    objectIndices[id] = objects.size() - 1;
    return id;
}

bool Scene::RemoveObject(uint32_t id)
{
    const auto found = objectIndices.find(id);
    if (found == objectIndices.end())
        return false;

    const size_t index = found->second;
    objectIndices.erase(found);
    if (index + 1 != objects.size())
    {
        objects[index] = std::move(objects.back());
        objectIndices[objects[index].id] = index;
    }
    objects.pop_back();
    return true;
}

SceneObject* Scene::FindObject(uint32_t id)
{
    const auto found = objectIndices.find(id);
    return found != objectIndices.end() ? &objects[found->second] : nullptr;
}

const SceneObject* Scene::FindObject(uint32_t id) const
{
    const auto found = objectIndices.find(id);
    return found != objectIndices.end() ? &objects[found->second] : nullptr;
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
    objectIndices.clear();
    lights.clear();
    sky = {};
    fog = {};
    nextObjectId = 1;
    nextLightId = 1;
}

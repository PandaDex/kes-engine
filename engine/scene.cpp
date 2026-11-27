#include "scene.h"
#include <algorithm>

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
    submittedPostProcessLayers.clear();
    particles.Clear();
    submittedPointLights.clear();
    ++exposureResetSerial;
    nextObjectId = 1;
    nextLightId = 1;
}

PostProcessSettings ResolvePostProcessLayers(const PostProcessSettings& base, const PostProcessLayer* const* layers, size_t count)
{
    std::vector<const PostProcessLayer*> sorted(layers, layers + count);
    std::stable_sort(sorted.begin(), sorted.end(), [](const PostProcessLayer* a, const PostProcessLayer* b)
        { return a->priority < b->priority; });

    PostProcessSettings out = base;
    for (const PostProcessLayer* layer : sorted)
    {
        const float w = std::clamp(layer->weight, 0.0f, 1.0f);
        if (w <= 0.0f)
            continue;
        const PostProcessSettings& in = layer->settings;
        const uint32_t mask = layer->overrides;
        auto blend = [&](uint32_t bit, float& value, float target)
        {
            if (mask & bit)
                value += (target - value) * w;
        };
        blend(PostProcessOverride_ExposureCompensation, out.exposureCompensation, in.exposureCompensation);
        if ((mask & PostProcessOverride_AutoExposure) && w > 0.5f)
            out.autoExposure = in.autoExposure;
        blend(PostProcessOverride_AutoExposureMin, out.autoExposureMin, in.autoExposureMin);
        blend(PostProcessOverride_AutoExposureMax, out.autoExposureMax, in.autoExposureMax);
        blend(PostProcessOverride_AdaptationSpeedUp, out.adaptationSpeedUp, in.adaptationSpeedUp);
        blend(PostProcessOverride_AdaptationSpeedDown, out.adaptationSpeedDown, in.adaptationSpeedDown);
        blend(PostProcessOverride_BloomIntensity, out.bloomIntensity, in.bloomIntensity);
        blend(PostProcessOverride_BloomRadius, out.bloomRadius, in.bloomRadius);
        blend(PostProcessOverride_VignetteIntensity, out.vignetteIntensity, in.vignetteIntensity);
        blend(PostProcessOverride_VignetteSmoothness, out.vignetteSmoothness, in.vignetteSmoothness);
        for (int i = 0; i < 3; ++i)
            blend(PostProcessOverride_VignetteColor, out.vignetteColor[i], in.vignetteColor[i]);
        if ((mask & PostProcessOverride_Tonemapper) && w > 0.5f)
            out.tonemapper = in.tonemapper;
        blend(PostProcessOverride_Temperature, out.temperature, in.temperature);
        blend(PostProcessOverride_Tint, out.tint, in.tint);
        blend(PostProcessOverride_Saturation, out.saturation, in.saturation);
        blend(PostProcessOverride_Contrast, out.contrast, in.contrast);
        for (int i = 0; i < 3; ++i)
        {
            blend(PostProcessOverride_ColorFilter, out.colorFilter[i], in.colorFilter[i]);
            blend(PostProcessOverride_Lift, out.lift[i], in.lift[i]);
            blend(PostProcessOverride_Gamma, out.gamma[i], in.gamma[i]);
            blend(PostProcessOverride_Gain, out.gain[i], in.gain[i]);
        }
        blend(PostProcessOverride_Sharpen, out.sharpen, in.sharpen);
        blend(PostProcessOverride_AmbientOcclusionIntensity, out.ambientOcclusionIntensity, in.ambientOcclusionIntensity);
        blend(PostProcessOverride_AmbientOcclusionRadius, out.ambientOcclusionRadius, in.ambientOcclusionRadius);
        blend(PostProcessOverride_ContactShadowLength, out.contactShadowLength, in.contactShadowLength);
        blend(PostProcessOverride_ReflectionIntensity, out.reflectionIntensity, in.reflectionIntensity);
        blend(PostProcessOverride_ReflectionMaxRoughness, out.reflectionMaxRoughness, in.reflectionMaxRoughness);
        blend(PostProcessOverride_ReflectionMaxDistance, out.reflectionMaxDistance, in.reflectionMaxDistance);
        blend(PostProcessOverride_DepthOfFieldFocusDistance, out.depthOfFieldFocusDistance, in.depthOfFieldFocusDistance);
        blend(PostProcessOverride_DepthOfFieldFocusRange, out.depthOfFieldFocusRange, in.depthOfFieldFocusRange);
        blend(PostProcessOverride_DepthOfFieldMaxBlur, out.depthOfFieldMaxBlur, in.depthOfFieldMaxBlur);
        blend(PostProcessOverride_MotionBlurIntensity, out.motionBlurIntensity, in.motionBlurIntensity);
    }
    if (out.autoExposureMax < out.autoExposureMin)
        out.autoExposureMax = out.autoExposureMin;
    return out;
}

PostProcessLayerId Scene::AddPostProcessLayer(const PostProcessLayer& layer)
{
    const PostProcessLayerId id = nextPostProcessLayerId++;
    postProcessLayers.emplace_back(id, layer);
    return id;
}

PostProcessLayer* Scene::FindPostProcessLayer(PostProcessLayerId id)
{
    for (auto& entry : postProcessLayers)
    {
        if (entry.first == id)
            return &entry.second;
    }
    return nullptr;
}

bool Scene::RemovePostProcessLayer(PostProcessLayerId id)
{
    for (auto it = postProcessLayers.begin(); it != postProcessLayers.end(); ++it)
    {
        if (it->first == id)
        {
            postProcessLayers.erase(it);
            return true;
        }
    }
    return false;
}

void Scene::SubmitPostProcessLayer(const PostProcessLayer& layer)
{
    submittedPostProcessLayers.push_back(layer);
}

void Scene::ClearSubmittedPostProcessLayers()
{
    submittedPostProcessLayers.clear();
}

void Scene::ResolvePostProcess()
{
    // Code layers are applied after submitted ones, so they win priority ties.
    std::vector<const PostProcessLayer*> layers;
    layers.reserve(postProcessLayers.size() + submittedPostProcessLayers.size());
    for (const PostProcessLayer& layer : submittedPostProcessLayers)
        layers.push_back(&layer);
    for (const auto& entry : postProcessLayers)
        layers.push_back(&entry.second);
    postProcess = ResolvePostProcessLayers(postProcessDefaults, layers.data(), layers.size());
}

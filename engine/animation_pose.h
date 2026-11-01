#pragma once
#include <string>
#include <vector>
#include "animation_clip.h"

struct KModelData;

void BuildAnimationWorldMatrices(const KModelData& model, const std::vector<AnimationTransform>& localPose, std::vector<float>& outWorldMatrices);
bool BuildAnimationSkinMatrices(const KModelData& model, const std::vector<AnimationTransform>& localPose, std::vector<float>& outSkinMatrices, std::string* outError = nullptr);

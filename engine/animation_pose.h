#pragma once
#include <string>
#include <vector>
#include "animation_clip.h"

struct KModelData;

void BuildAnimationWorldMatrices(const KModelData& model, const std::vector<AnimationTransform>& localPose, std::vector<float>& outWorldMatrices);
// Matrices are row-major for row vectors (v * M), translation in elements 12..14.
void BuildTransformMatrix(const float translation[3], const float rotation[4], const float scale[3], float outMatrix[16]);
void MultiplyMatrices(const float first[16], const float then[16], float outMatrix[16]);
void DecomposeMatrix(const float matrix[16], float outTranslation[3], float outRotation[4], float outScale[3]);
// Model-space matrix of a transform attached to a bone (boneIndex -1: the model root),
// given node world matrices from BuildAnimationWorldMatrices.
void BuildBoneAttachmentMatrix(const KModelData& model, const std::vector<float>& nodeWorldMatrices, int32_t boneIndex,
                               const float translation[3], const float rotation[4], const float scale[3], float outMatrix[16]);
bool BuildAnimationSkinMatrices(const KModelData& model, const std::vector<AnimationTransform>& localPose, std::vector<float>& outSkinMatrices, std::string* outError = nullptr);

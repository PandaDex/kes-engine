// Eye adaptation pass 1: log2 luminance histogram of the HDR scene.
#include "exposure_common.slang"

[[vk::binding(0, 0)]]
Texture2D<float4> uSceneColor : register(t0);

groupshared uint gsBins[kHistogramBins];

uint LuminanceToBin(float luminance)
{
    float t = saturate((log2(max(luminance, 1e-6)) - uExposure.params0.x) * uExposure.params0.y);
    return luminance > 1e-6 ? uint(t * 254.0 + 1.0) : 0;
}

// Branch-free atomics: fxc fails ("flattened side effect") on atomics under a branch.
[shader("compute")]
[numthreads(16, 16, 1)]
void csMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    gsBins[index] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint2 size = uint2(uExposure.params0.zw);
    uint2 pixel = min(id.xy, size - 1);
    float3 color = uSceneColor.Load(int3(pixel, 0)).rgb;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    uint inside = (id.x < size.x && id.y < size.y) ? 1 : 0;
    InterlockedAdd(gsBins[LuminanceToBin(luminance)], inside);
    GroupMemoryBarrierWithGroupSync();

    InterlockedAdd(uHistogram[index], gsBins[index]);
}

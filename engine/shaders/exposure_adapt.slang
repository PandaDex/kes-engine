// Eye adaptation pass 2: the average log2 luminance of the histogram (darkest
// and brightest pixels left out) is adapted towards over time and turned into
// the exposure that maps it to middle grey. Clears the histogram for the next frame.
#include "exposure_common.slang"

[[vk::binding(2, 0)]]
RWStructuredBuffer<float> uState : register(u1);   // [0] = exposure, [1] = adapted log2 luminance, [2] = 1 once initialized

groupshared uint gsBins[kHistogramBins];

static const float kLowPercent = 0.4;
static const float kHighPercent = 0.95;
static const float kMiddleGrey = 0.18;

float BinToLog2(uint bin)
{
    if (bin == 0)
        return uExposure.params0.x;
    return uExposure.params0.x + ((float(bin) - 0.5) / 254.0) / uExposure.params0.y;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csMain(uint index : SV_GroupIndex)
{
    gsBins[index] = uHistogram[index];
    uHistogram[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
        return;

    float total = 0.0;
    for (uint i = 0; i < kHistogramBins; ++i)
        total += float(gsBins[i]);

    // Average over the pixels between the low and high percentiles.
    float lowCut = total * kLowPercent;
    float highCut = total * kHighPercent;
    float cumulative = 0.0;
    float sum = 0.0;
    float weight = 0.0;
    for (uint bin = 0; bin < kHistogramBins; ++bin)
    {
        float count = float(gsBins[bin]);
        float inside = max(min(cumulative + count, highCut) - max(cumulative, lowCut), 0.0);
        cumulative += count;
        sum += inside * BinToLog2(bin);
        weight += inside;
    }

    float target = weight > 0.0 ? sum / weight : log2(kMiddleGrey);
    target = clamp(target, uExposure.params2.x, uExposure.params2.y);

    float adapted = target;
    if (uExposure.params1.w < 0.5 && uState[2] > 0.5)
    {
        float previous = uState[1];
        float speed = target > previous ? uExposure.params1.y : uExposure.params1.z;
        adapted = previous + (target - previous) * (1.0 - exp(-uExposure.params1.x * speed));
    }
    uState[0] = kMiddleGrey / exp2(adapted);
    uState[1] = adapted;
    uState[2] = 1.0;
}

// Eye adaptation: constants and bindings shared by the histogram and adapt passes.
#pragma once

struct ExposureConstants
{
    float4 params0;   // x = histogram min log2 luminance, y = 1 / log2 range, z = width, w = height
    float4 params1;   // x = delta time, y = speed up, z = speed down, w = 1 snap
    float4 params2;   // x = adaptation min log2 luminance, y = max
};

[[vk::push_constant]]
ConstantBuffer<ExposureConstants> uExposure;   // b0 on D3D

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> uHistogram : register(u0);   // bin 0 = below the range

static const uint kHistogramBins = 256;

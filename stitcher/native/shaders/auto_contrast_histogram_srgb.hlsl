#include "linear_to_srgb.hlsli"

cbuffer HistogramConstants : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> linear_rgb : register(t0);
Buffer<uint> coverage : register(t1);
RWStructuredBuffer<uint> histogram : register(u0);

[numthreads(64, 1, 1)]
void build_auto_contrast_histogram_srgb(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count || coverage[index] == 0u || !all(isfinite(linear_rgb[index])))
        return;
    const float3 encoded = linear_to_normalized_srgb(linear_rgb[index]);
    const float luminance = saturate(dot(encoded, float3(0.2126f, 0.7152f, 0.0722f)));
    const uint bin = min(4095u, (uint)floor(luminance * 4096.0f));
    InterlockedAdd(histogram[bin], 1u);
}

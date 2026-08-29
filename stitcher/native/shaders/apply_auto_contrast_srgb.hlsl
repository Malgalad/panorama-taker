#include "linear_to_srgb.hlsli"

cbuffer ApplyAutoContrastConstants : register(b0)
{
    uint pixel_count;
    uint apply_levels;
    float black_level;
    float white_level;
};

StructuredBuffer<float3> linear_rgb : register(t0);
RWStructuredBuffer<float3> normalized_srgb : register(u0);

[numthreads(64, 1, 1)]
void apply_auto_contrast_srgb(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    float3 encoded = linear_to_normalized_srgb(linear_rgb[index]);
    if (apply_levels != 0u && white_level > black_level)
        encoded = (encoded - black_level) / (white_level - black_level);
    normalized_srgb[index] = saturate(encoded);
}

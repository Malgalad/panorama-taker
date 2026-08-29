#include "linear_to_srgb.hlsli"

cbuffer ConvertLinearSrgbConstants : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> linear_rgb : register(t0);
RWStructuredBuffer<float3> normalized_srgb : register(u0);

[numthreads(64, 1, 1)]
void convert_linear_srgb(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index < pixel_count)
        normalized_srgb[index] = linear_to_normalized_srgb(linear_rgb[index]);
}

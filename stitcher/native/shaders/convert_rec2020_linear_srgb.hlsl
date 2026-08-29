#include "rec2020_to_linear_srgb.hlsli"

cbuffer Parameters : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> tone_mapped_rec2020 : register(t0);
RWStructuredBuffer<float3> linear_srgb : register(u0);

[numthreads(64, 1, 1)]
void convert_rec2020_linear_srgb(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x < pixel_count)
        linear_srgb[thread_id.x] = rec2020_to_linear_srgb(tone_mapped_rec2020[thread_id.x]);
}

#include "tone_map_rec2020.hlsli"

cbuffer Parameters : register(b0)
{
    uint pixel_count;
    float reference_white_nits;
};

StructuredBuffer<float3> linear_rec2020 : register(t0);
RWStructuredBuffer<float3> tone_mapped_rec2020 : register(u0);

[numthreads(64, 1, 1)]
void apply_tone_map_rec2020(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x < pixel_count)
        tone_mapped_rec2020[thread_id.x] =
            tone_map_rec2020(linear_rec2020[thread_id.x], reference_white_nits);
}

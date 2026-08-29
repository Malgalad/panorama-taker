cbuffer ExposurePairClassifyConstants : register(b0)
{
    uint sample_count;
    uint transfer_function;
};

StructuredBuffer<float3> first_samples : register(t0);
StructuredBuffer<float3> second_samples : register(t1);
Buffer<uint> geometric_overlap : register(t2);
RWStructuredBuffer<float2> pair_luminance : register(u0);
RWStructuredBuffer<uint> accepted : register(u1);
RWStructuredBuffer<uint> pair_counts : register(u2);

[numthreads(64, 1, 1)]
void classify_resident_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    if (geometric_overlap[index] != 0u)
        InterlockedAdd(pair_counts[4], 1u);
    const float3 first_rgb = first_samples[index];
    const float3 second_rgb = second_samples[index];
    const float first = dot(first_rgb, float3(0.2126f, 0.7152f, 0.0722f));
    const float second = dot(second_rgb, float3(0.2126f, 0.7152f, 0.0722f));
    pair_luminance[index] = float2(first, second);
    const bool clipped = transfer_function != 3u &&
        (any(first_rgb >= 0.995f) || any(second_rgb >= 0.995f));
    accepted[index] = geometric_overlap[index] != 0u && isfinite(first) && isfinite(second) &&
        first > 1.0e-5f && second > 1.0e-5f && !clipped ? 1u : 0u;
}

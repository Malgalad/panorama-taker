cbuffer ExposurePairClassifyConstants : register(b0)
{
    uint sample_count;
    uint transfer_function;
};

Buffer<float> sampled_pairs : register(t0);
Buffer<uint> geometric_overlap : register(t1);
RWStructuredBuffer<float2> pair_luminance : register(u0);
RWStructuredBuffer<uint> accepted : register(u1);

[numthreads(64, 1, 1)]
void classify_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    const uint base = index * 6u;
    const float3 first_rgb = float3(sampled_pairs[base], sampled_pairs[base + 1u], sampled_pairs[base + 2u]);
    const float3 second_rgb = float3(sampled_pairs[base + 3u], sampled_pairs[base + 4u], sampled_pairs[base + 5u]);
    const float first = dot(first_rgb,
        float3(0.2126f, 0.7152f, 0.0722f));
    const float second = dot(second_rgb,
        float3(0.2126f, 0.7152f, 0.0722f));
    pair_luminance[index] = float2(first, second);
    const bool clipped = transfer_function != 3u &&
        (any(first_rgb >= 0.995f) || any(second_rgb >= 0.995f));
    accepted[index] = geometric_overlap[index] != 0u && isfinite(first) && isfinite(second) &&
        first > 1.0e-5f && second > 1.0e-5f && !clipped ? 1u : 0u;
}

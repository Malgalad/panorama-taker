cbuffer PairReductionConstants : register(b0)
{
    uint sample_count;
    uint padded_count;
};

StructuredBuffer<float> sorted_ratios : register(t0);
Buffer<uint> retained : register(t1);
StructuredBuffer<float2> trim_bounds : register(t2);
RWStructuredBuffer<uint4> summary : register(u0);

[numthreads(1, 1, 1)]
void summarize_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    uint valid_count = 0u;
    while (valid_count < sample_count && isfinite(sorted_ratios[valid_count]))
        ++valid_count;
    uint inlier_count = 0u;
    for (uint index = 0u; index < sample_count; ++index)
        inlier_count += retained[index] != 0u ? 1u : 0u;
    float median = asfloat(0x7FC00000u);
    if (inlier_count != 0u)
    {
        const float lower_bound = trim_bounds[0].x;
        uint start = 0u;
        while (start < valid_count && sorted_ratios[start] < lower_bound)
            ++start;
        const uint span = inlier_count - 1u;
        const uint lower = start + span / 2u;
        const uint upper = start + (span + 1u) / 2u;
        median = 0.5f * (sorted_ratios[lower] + sorted_ratios[upper]);
    }
    summary[0] = uint4(asuint(median), valid_count, inlier_count, 0u);
}

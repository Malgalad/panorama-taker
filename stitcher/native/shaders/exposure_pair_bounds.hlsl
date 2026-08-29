cbuffer ExposurePairBoundsConstants : register(b0)
{
    uint sample_count;
};

StructuredBuffer<float> sorted_ratios : register(t0);
Buffer<uint> accepted : register(t1);
RWStructuredBuffer<float2> trim_bounds : register(u0);

float interpolate_rank(uint lower, uint remainder, uint denominator)
{
    const uint upper = lower + (remainder != 0u ? 1u : 0u);
    const float fraction = float(remainder) / float(denominator);
    return sorted_ratios[lower] * (1.0f - fraction) + sorted_ratios[upper] * fraction;
}

[numthreads(1, 1, 1)]
void extract_exposure_pair_bounds(uint3 thread_id : SV_DispatchThreadID)
{
    uint accepted_count = 0u;
    for (uint index = 0u; index < sample_count; ++index)
        accepted_count += accepted[index] != 0u ? 1u : 0u;
    if (accepted_count == 0u)
    {
        const float missing = asfloat(0x7FC00000u);
        trim_bounds[0] = float2(missing, missing);
        return;
    }
    const uint span = accepted_count - 1u;
    const uint lower_rank = span / 10u;
    const uint lower_remainder = span % 10u;
    const uint upper_units = (span % 10u) * 9u;
    const uint upper_rank = (span / 10u) * 9u + upper_units / 10u;
    const uint upper_remainder = upper_units % 10u;
    trim_bounds[0] = float2(
        interpolate_rank(lower_rank, lower_remainder, 10u),
        interpolate_rank(upper_rank, upper_remainder, 10u));
}

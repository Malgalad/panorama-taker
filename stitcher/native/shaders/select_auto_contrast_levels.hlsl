StructuredBuffer<uint> histogram : register(t0);
RWStructuredBuffer<float2> levels : register(u0);

[numthreads(1, 1, 1)]
void select_auto_contrast_levels(uint3 thread_id : SV_DispatchThreadID)
{
    uint total = 0u;
    for (uint sum_bin = 0u; sum_bin < 4096u; ++sum_bin)
        total += histogram[sum_bin];
    if (total == 0u)
    {
        levels[0] = float2(0.0f, 1.0f);
        return;
    }
    const float black_rank = 0.005f * float(total - 1u);
    const float white_rank = 0.995f * float(total - 1u);
    uint cumulative = 0u;
    float black = 0.0f;
    float white = 1.0f;
    bool black_found = false;
    for (uint percentile_bin = 0u; percentile_bin < 4096u; ++percentile_bin)
    {
        const uint previous = cumulative;
        cumulative += histogram[percentile_bin];
        if (!black_found && float(cumulative) >= black_rank + 1.0f)
        {
            black = (float(percentile_bin) +
                     (black_rank - float(previous)) / float(histogram[percentile_bin])) /
                4096.0f;
            black_found = true;
        }
        if (float(cumulative) >= white_rank + 1.0f)
        {
            white = (float(percentile_bin) +
                     (white_rank - float(previous)) / float(histogram[percentile_bin])) /
                4096.0f;
            break;
        }
    }
    levels[0] = white - black < 1.0f / 4096.0f ? float2(0.0f, 0.0f) : float2(black, white);
}

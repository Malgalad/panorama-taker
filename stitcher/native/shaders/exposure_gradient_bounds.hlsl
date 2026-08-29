cbuffer ExposureGradientBoundsConstants : register(b0)
{
    uint sample_count;
    uint padded_count;
    uint unused_bounds;
};

StructuredBuffer<float> sorted_gradients : register(t0);
RWStructuredBuffer<float2> gradient_limits : register(u0);

float channel_p90(const uint channel)
{
    const uint start = channel * padded_count;
    uint count = 0u;
    while (count < sample_count && isfinite(sorted_gradients[start + count]))
        ++count;
    float result = asfloat(0x7FC00000u);
    if (count != 0u)
    {
        const uint span = count - 1u;
        const uint units = (span % 10u) * 9u;
        const uint lower = (span / 10u) * 9u + units / 10u;
        const uint remainder = units % 10u;
        const uint upper = lower + (remainder != 0u ? 1u : 0u);
        const float fraction = float(remainder) / 10.0f;
        result = sorted_gradients[start + lower] * (1.0f - fraction) +
            sorted_gradients[start + upper] * fraction;
    }
    return result;
}

[numthreads(1, 1, 1)]
void extract_exposure_gradient_limits(uint3 thread_id : SV_DispatchThreadID)
{
    gradient_limits[0] = float2(channel_p90(0u), channel_p90(1u));
}

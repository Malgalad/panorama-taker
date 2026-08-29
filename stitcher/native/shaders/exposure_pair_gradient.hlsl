cbuffer ExposurePairGradientConstants : register(b0)
{
    uint sample_width;
    uint sample_height;
};

StructuredBuffer<float2> pair_luminance : register(t0);
RWStructuredBuffer<float2> gradients : register(u0);

float2 logged_luminance(const uint x, const uint y)
{
    return log(max(pair_luminance[y * sample_width + x], float2(1.0e-5f, 1.0e-5f)));
}

[numthreads(64, 1, 1)]
void gradient_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    const uint sample_count = sample_width * sample_height;
    if (index >= sample_count)
        return;
    const uint x = index % sample_width;
    const uint y = index / sample_width;
    const uint left_x = x == 0u ? min(1u, sample_width - 1u) : x - 1u;
    const uint right_x = x == sample_width - 1u ? max(sample_width - 2u, 0u) : x + 1u;
    const uint top_y = y == 0u ? min(1u, sample_height - 1u) : y - 1u;
    const uint bottom_y = y == sample_height - 1u ? max(sample_height - 2u, 0u) : y + 1u;
    const float2 top_left = logged_luminance(left_x, top_y);
    const float2 top = logged_luminance(x, top_y);
    const float2 top_right = logged_luminance(right_x, top_y);
    const float2 left = logged_luminance(left_x, y);
    const float2 right = logged_luminance(right_x, y);
    const float2 bottom_left = logged_luminance(left_x, bottom_y);
    const float2 bottom = logged_luminance(x, bottom_y);
    const float2 bottom_right = logged_luminance(right_x, bottom_y);
    const float2 horizontal = -top_left + top_right - 2.0f * left + 2.0f * right - bottom_left + bottom_right;
    const float2 vertical = -top_left - 2.0f * top - top_right + bottom_left + 2.0f * bottom + bottom_right;
    gradients[index] = sqrt(horizontal * horizontal + vertical * vertical);
}

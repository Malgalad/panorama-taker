cbuffer ExposureProxyConstants : register(b0)
{
    uint frame_count;
    uint source_width;
    uint source_height;
    uint proxy_width;
    uint proxy_height;
    uint source_row_stride_elements;
    uint source_frame_offset_elements;
    uint transfer_function;
};

Buffer<float> sources : register(t0);
RWStructuredBuffer<float3> proxies : register(u0);

[numthreads(64, 1, 1)]
void build_float32_exposure_proxies(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    const uint pixels_per_frame = proxy_width * proxy_height;
    if (index >= frame_count * pixels_per_frame)
        return;
    const uint frame = index / pixels_per_frame;
    const uint pixel = index % pixels_per_frame;
    const uint x = pixel % proxy_width;
    const uint y = pixel / proxy_width;
    const float left = x * (float)source_width / proxy_width;
    const float right = (x + 1) * (float)source_width / proxy_width;
    const float top = y * (float)source_height / proxy_height;
    const float bottom = (y + 1) * (float)source_height / proxy_height;
    float3 sum = 0.0f;
    float total = 0.0f;
    for (uint sample_y = (uint)floor(top); sample_y < min((uint)ceil(bottom), source_height); ++sample_y)
    {
        const float weight_y = max(0.0f, min(bottom, sample_y + 1.0f) - max(top, (float)sample_y));
        for (uint sample_x = (uint)floor(left); sample_x < min((uint)ceil(right), source_width); ++sample_x)
        {
            const uint source = frame * source_frame_offset_elements + sample_y * source_row_stride_elements + sample_x * 3u;
            const float weight = weight_y * max(0.0f, min(right, sample_x + 1.0f) - max(left, (float)sample_x));
            float3 value = float3(sources[source], sources[source + 1], sources[source + 2]);
            if (transfer_function == 1u)
                value = max(value, 0.0f) <= 0.04045f ? max(value, 0.0f) / 12.92f : pow(max((max(value, 0.0f) + 0.055f) / 1.055f, 0.0f), 2.4f);
            else if (transfer_function == 2u)
            {
                const float3 powered = pow(max(value, 0.0f), 32.0f / 2523.0f);
                value = pow(max((powered - 3424.0f / 4096.0f) /
                    max(2413.0f / 128.0f - 2392.0f / 128.0f * powered, 1.17549435e-38f), 0.0f), 16384.0f / 2610.0f);
            }
            sum += weight * value;
            total += weight;
        }
    }
    proxies[index] = sum / total;
}

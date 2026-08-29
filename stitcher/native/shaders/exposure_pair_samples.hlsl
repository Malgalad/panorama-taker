cbuffer ExposurePairSampleConstants : register(b0)
{
    uint first_frame_index;
    uint second_frame_index;
    uint sample_count;
    uint proxy_width;
    uint proxy_height;
};

Buffer<float> proxies : register(t0);
StructuredBuffer<float4> paired_coordinates : register(t1);
RWStructuredBuffer<float3> first_samples : register(u0);
RWStructuredBuffer<float3> second_samples : register(u1);

float3 sample_proxy(const uint frame, const float2 coordinate)
{
    const uint x0 = (uint)floor(coordinate.x);
    const uint y0 = (uint)floor(coordinate.y);
    const uint x1 = min(x0 + 1u, proxy_width - 1u);
    const uint y1 = min(y0 + 1u, proxy_height - 1u);
    const float wx = coordinate.x - x0;
    const float wy = coordinate.y - y0;
    const uint base = frame * proxy_width * proxy_height * 3u;
    const uint p00 = base + (y0 * proxy_width + x0) * 3u;
    const uint p10 = base + (y0 * proxy_width + x1) * 3u;
    const uint p01 = base + (y1 * proxy_width + x0) * 3u;
    const uint p11 = base + (y1 * proxy_width + x1) * 3u;
    return (1.0f - wx) * (1.0f - wy) * float3(proxies[p00], proxies[p00 + 1u], proxies[p00 + 2u]) +
        wx * (1.0f - wy) * float3(proxies[p10], proxies[p10 + 1u], proxies[p10 + 2u]) +
        (1.0f - wx) * wy * float3(proxies[p01], proxies[p01 + 1u], proxies[p01 + 2u]) +
        wx * wy * float3(proxies[p11], proxies[p11 + 1u], proxies[p11 + 2u]);
}

[numthreads(64, 1, 1)]
void sample_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    const float4 coordinates = paired_coordinates[index];
    first_samples[index] = sample_proxy(first_frame_index, coordinates.xy);
    second_samples[index] = sample_proxy(second_frame_index, coordinates.zw);
}

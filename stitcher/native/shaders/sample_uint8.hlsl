cbuffer SampleConstants : register(b0)
{
    uint source_width;
    uint source_height;
    uint source_row_stride_bytes;
    uint source_frame_offset_bytes;
};

Buffer<uint> source_bytes : register(t0);
StructuredBuffer<float2> coordinates : register(t1);
RWStructuredBuffer<float3> sampled_rgb : register(u0);

float3 load_source_rgb(const uint2 coordinate)
{
    const uint source_offset = source_frame_offset_bytes + coordinate.y * source_row_stride_bytes + coordinate.x * 3u;
    return float3(
        source_bytes[source_offset], source_bytes[source_offset + 1u], source_bytes[source_offset + 2u]) *
        (1.0f / 255.0f);
}

[numthreads(64, 1, 1)]
void sample_uint8(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= coordinates.Length)
        return;
    const float2 coordinate = clamp(coordinates[index], float2(0.0f, 0.0f), float2(source_width - 1u, source_height - 1u));
    const uint2 lower = uint2(floor(coordinate));
    const uint2 upper = min(lower + 1u, uint2(source_width - 1u, source_height - 1u));
    const float2 fraction = frac(coordinate);
    const float3 top = lerp(load_source_rgb(uint2(lower.x, lower.y)), load_source_rgb(uint2(upper.x, lower.y)), fraction.x);
    const float3 bottom = lerp(load_source_rgb(uint2(lower.x, upper.y)), load_source_rgb(uint2(upper.x, upper.y)), fraction.x);
    sampled_rgb[index] = lerp(top, bottom, fraction.y);
}

#include "decode_transfer.hlsli"

cbuffer CandidateConstants : register(b0)
{
    uint output_width;
    uint output_height;
    uint row_start;
    uint row_count;
    uint source_width;
    uint source_height;
    uint source_row_stride_bytes;
    uint source_frame_offset_bytes;
    float4 latitude_padding;
    float4 rotation_row_0;
    float4 rotation_row_1;
    float4 rotation_row_2;
    float4 source_camera;
    float4 global_gain_padding;
    float rectilinear_output;
    float output_vertical_fov_degrees;
    uint transfer_function;
    uint transfer_padding;
};

Buffer<uint> source_values : register(t0);
RWStructuredBuffer<float3> candidate_rgb : register(u0);
RWByteAddressBuffer validity_bits : register(u1);
RWStructuredBuffer<float> candidate_edge_distance : register(u2);

float3 load_source_rgb(const uint2 coordinate)
{
    const uint offset = source_frame_offset_bytes + coordinate.y * source_row_stride_bytes + coordinate.x * 3u;
    const float3 encoded = float3(source_values[offset], source_values[offset + 1u], source_values[offset + 2u]) * (1.0f / 255.0f);
    return decode_source_transfer(encoded, transfer_function);
}

[numthreads(8, 8, 1)]
void generate_uint8_candidate(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x >= output_width || thread_id.y >= row_count)
        return;
    float3 world_ray;
    if (rectilinear_output != 0.0f)
    {
        const float focal_x = output_width * 0.5f;
        const float focal_y = output_height / (2.0f * tan(output_vertical_fov_degrees * 0.00872664625997165f));
        world_ray = normalize(float3(
            (thread_id.x - (output_width - 1) * 0.5f) / focal_x,
            ((output_height - 1) * 0.5f - (row_start + thread_id.y)) / focal_y, 1.0f));
    }
    else
    {
        const float longitude = ((thread_id.x + 0.5f) / output_width - 0.5f) * 6.283185307179586f;
        const float latitude =
            (0.5f - (row_start + thread_id.y + 0.5f) / output_height) * latitude_padding.x * 0.0174532925199433f;
        const float cosine = cos(latitude);
        world_ray = float3(cosine * sin(longitude), sin(latitude), cosine * cos(longitude));
    }
    const float3 camera_ray = float3(
        dot(world_ray, float3(rotation_row_0.x, rotation_row_1.x, rotation_row_2.x)),
        dot(world_ray, float3(rotation_row_0.y, rotation_row_1.y, rotation_row_2.y)),
        dot(world_ray, float3(rotation_row_0.z, rotation_row_1.z, rotation_row_2.z)));
    const float safe_z = abs(camera_ray.z) > 1.0e-8f ? camera_ray.z : 1.0f;
    const float2 projected = float2(
        (source_camera.x - 1.0f) * 0.5f + source_camera.z * camera_ray.x / safe_z,
        (source_camera.y - 1.0f) * 0.5f - source_camera.w * camera_ray.y / safe_z);
    const uint index = thread_id.y * output_width + thread_id.x;
    const bool valid = camera_ray.z > 0.0f && projected.x >= -0.5f && projected.x <= source_camera.x - 0.5f &&
        projected.y >= -0.5f && projected.y <= source_camera.y - 0.5f;
    if (valid)
    {
        uint previous_bits;
        validity_bits.InterlockedOr((index / 32u) * 4u, 1u << (index & 31u), previous_bits);
    }
    const float2 coordinate = clamp(projected, float2(0.0f, 0.0f), source_camera.xy - 1.0f);
    candidate_edge_distance[index] = min(
        min(coordinate.x, coordinate.y), min(source_camera.x - 1.0f - coordinate.x, source_camera.y - 1.0f - coordinate.y));
    const uint2 lower = uint2(floor(coordinate));
    const uint2 upper = min(lower + 1u, uint2(source_width - 1u, source_height - 1u));
    const float2 fraction = frac(coordinate);
    const float3 top = lerp(load_source_rgb(uint2(lower.x, lower.y)), load_source_rgb(uint2(upper.x, lower.y)), fraction.x);
    const float3 bottom = lerp(load_source_rgb(uint2(lower.x, upper.y)), load_source_rgb(uint2(upper.x, upper.y)), fraction.x);
    candidate_rgb[index] = lerp(top, bottom, fraction.y) * global_gain_padding.x;
}

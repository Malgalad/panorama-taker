cbuffer RayConstants : register(b0)
{
    uint output_width;
    uint output_height;
    uint row_start;
    uint row_count;
    float4 latitude_padding;
    float4 rotation_row_0;
    float4 rotation_row_1;
    float4 rotation_row_2;
    float4 source_camera;
};

RWStructuredBuffer<float3> camera_rays : register(u0);
RWStructuredBuffer<float2> projected_coordinates : register(u1);
RWByteAddressBuffer validity_bits : register(u2);

[numthreads(8, 8, 1)]
void generate_rays(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x >= output_width || thread_id.y >= row_count)
        return;
    const float longitude = ((thread_id.x + 0.5f) / output_width - 0.5f) * 6.283185307179586f;
    const float latitude =
        (0.5f - (row_start + thread_id.y + 0.5f) / output_height) * latitude_padding.x * 0.0174532925199433f;
    const float cosine = cos(latitude);
    const float3 world_ray = float3(cosine * sin(longitude), sin(latitude), cosine * cos(longitude));
    const float3 camera_ray = float3(
        dot(world_ray, float3(rotation_row_0.x, rotation_row_1.x, rotation_row_2.x)),
        dot(world_ray, float3(rotation_row_0.y, rotation_row_1.y, rotation_row_2.y)),
        dot(world_ray, float3(rotation_row_0.z, rotation_row_1.z, rotation_row_2.z)));
    const uint index = thread_id.y * output_width + thread_id.x;
    camera_rays[index] = camera_ray;
    const float safe_z = abs(camera_ray.z) > 1.0e-8f ? camera_ray.z : 1.0f;
    const float2 projected_coordinate = float2(
        (source_camera.x - 1.0f) * 0.5f + source_camera.z * camera_ray.x / safe_z,
        (source_camera.y - 1.0f) * 0.5f - source_camera.w * camera_ray.y / safe_z);
    if (camera_ray.z > 0.0f && projected_coordinate.x >= -0.5f &&
        projected_coordinate.x <= source_camera.x - 0.5f && projected_coordinate.y >= -0.5f &&
        projected_coordinate.y <= source_camera.y - 0.5f)
    {
        uint previous_bits;
        validity_bits.InterlockedOr((index / 32u) * 4u, 1u << (index & 31u), previous_bits);
    }
    projected_coordinates[index] = clamp(projected_coordinate, float2(0.0f, 0.0f), source_camera.xy - 1.0f);
}

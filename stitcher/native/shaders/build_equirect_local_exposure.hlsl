cbuffer LocalExposureConstants : register(b0)
{
    uint field_width;
    uint field_height;
    uint output_width;
    uint output_height;
    uint source_width;
    uint source_height;
    uint2 padding;
    float4 latitude_padding;
    float4 rotation_row_0;
    float4 rotation_row_1;
    float4 rotation_row_2;
    float4 source_camera;
    float4 log_gain_padding;
    float4 projection_padding;
};

RWStructuredBuffer<float> local_field : register(u0);

[numthreads(8, 8, 1)]
void build_equirect_local_exposure(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x >= field_width || thread_id.y >= field_height)
        return;
    const float output_x = (thread_id.x + 0.5f) * output_width / field_width - 0.5f;
    const float output_y = (thread_id.y + 0.5f) * output_height / field_height - 0.5f;
    float3 world_ray;
    if (projection_padding.x != 0.0f)
    {
        const float focal_x = output_width * 0.5f;
        const float focal_y = output_height / (2.0f * tan(projection_padding.y * 0.00872664625997165f));
        world_ray = normalize(float3(
            (output_x - (output_width - 1) * 0.5f) / focal_x,
            ((output_height - 1) * 0.5f - output_y) / focal_y, 1.0f));
    }
    else
    {
        const float longitude = ((output_x + 0.5f) / output_width - 0.5f) * 6.283185307179586f;
        const float latitude = (0.5f - (output_y + 0.5f) / output_height) * latitude_padding.x * 0.0174532925199433f;
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
    const bool valid = camera_ray.z > 0.0f && projected.x >= -0.5f && projected.x <= source_camera.x - 0.5f &&
        projected.y >= -0.5f && projected.y <= source_camera.y - 0.5f;
    local_field[thread_id.y * field_width + thread_id.x] = valid ? log_gain_padding.x : 0.0f;
}

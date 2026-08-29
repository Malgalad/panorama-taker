cbuffer ExposurePairConstants : register(b0)
{
    uint first_frame_index;
    uint second_frame_index;
    uint sample_width;
    uint sample_height;
    uint proxy_width;
    uint proxy_height;
    float latitude_span_degrees;
    float horizontal_fov_degrees;
    float vertical_fov_degrees;
};

Buffer<float> rotations : register(t0);
RWStructuredBuffer<float4> paired_coordinates : register(u0);
RWStructuredBuffer<uint> overlap : register(u1);

bool project(const float3 world, const uint frame, out float2 coordinate)
{
    const uint base = frame * 9u;
    const float3 camera = float3(
        dot(world, float3(rotations[base], rotations[base + 3u], rotations[base + 6u])),
        dot(world, float3(rotations[base + 1u], rotations[base + 4u], rotations[base + 7u])),
        dot(world, float3(rotations[base + 2u], rotations[base + 5u], rotations[base + 8u])));
    const float safe_z = abs(camera.z) > 1.0e-8f ? camera.z : 1.0f;
    const float focal_x = proxy_width / (2.0f * tan(horizontal_fov_degrees * 0.00872664625997165f));
    const float focal_y = proxy_height / (2.0f * tan(vertical_fov_degrees * 0.00872664625997165f));
    const float2 projected = float2(
        (proxy_width - 1.0f) * 0.5f + focal_x * camera.x / safe_z,
        (proxy_height - 1.0f) * 0.5f - focal_y * camera.y / safe_z);
    coordinate = clamp(projected, float2(0.0f, 0.0f), float2(proxy_width - 1u, proxy_height - 1u));
    return camera.z > 0.0f && projected.x >= -0.5f && projected.x <= proxy_width - 0.5f &&
        projected.y >= -0.5f && projected.y <= proxy_height - 0.5f;
}

[numthreads(64, 1, 1)]
void project_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_width * sample_height)
        return;
    const uint x = index % sample_width;
    const uint y = index / sample_width;
    const float longitude = ((x + 0.5f) / sample_width - 0.5f) * 6.283185307179586f;
    const float latitude = (0.5f - (y + 0.5f) / sample_height) * latitude_span_degrees * 0.0174532925199433f;
    const float cosine = cos(latitude);
    const float3 world = float3(cosine * sin(longitude), sin(latitude), cosine * cos(longitude));
    float2 first_coordinate, second_coordinate;
    const bool first_valid = project(world, first_frame_index, first_coordinate);
    const bool second_valid = project(world, second_frame_index, second_coordinate);
    paired_coordinates[index] = float4(first_coordinate, second_coordinate);
    overlap[index] = first_valid && second_valid ? 1u : 0u;
}

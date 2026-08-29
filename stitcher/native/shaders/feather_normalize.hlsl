cbuffer FeatherNormalizeConstants : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> accumulator_rgb : register(t0);
StructuredBuffer<float> accumulator_weight : register(t1);
RWStructuredBuffer<float3> normalized_rgb : register(u0);

[numthreads(64, 1, 1)]
void feather_normalize(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    const float weight = accumulator_weight[index];
    normalized_rgb[index] = weight > 0.0f ? accumulator_rgb[index] / weight : accumulator_rgb[index];
}

cbuffer FeatherAccumulateConstants : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> candidate_rgb : register(t0);
StructuredBuffer<float> candidate_weight : register(t1);
StructuredBuffer<float3> accumulator_rgb : register(t2);
StructuredBuffer<float> accumulator_weight : register(t3);
RWStructuredBuffer<float3> result_rgb : register(u0);
RWStructuredBuffer<float> result_weight : register(u1);

[numthreads(64, 1, 1)]
void feather_accumulate(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    const float weight = candidate_weight[index];
    result_rgb[index] = accumulator_rgb[index] + candidate_rgb[index] * weight;
    result_weight[index] = accumulator_weight[index] + weight;
}

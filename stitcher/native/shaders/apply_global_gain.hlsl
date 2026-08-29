cbuffer GlobalGainConstants : register(b0)
{
    uint pixel_count;
    float global_gain;
};

StructuredBuffer<float3> candidate_rgb : register(t0);
RWStructuredBuffer<float3> adjusted_rgb : register(u0);

[numthreads(64, 1, 1)]
void apply_global_gain(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    adjusted_rgb[index] = candidate_rgb[index] * global_gain;
}

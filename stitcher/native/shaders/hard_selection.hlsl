cbuffer HardSelectionConstants : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> candidate_rgb : register(t0);
ByteAddressBuffer candidate_validity_bits : register(t1);
StructuredBuffer<float> candidate_edge_distance : register(t2);
StructuredBuffer<float3> prior_rgb : register(t3);
StructuredBuffer<float> prior_weight : register(t4);
RWStructuredBuffer<float3> selected_rgb : register(u0);
RWStructuredBuffer<float> selected_weight : register(u1);
RWBuffer<uint> coverage : register(u2);

[numthreads(64, 1, 1)]
void hard_select(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    const bool candidate_is_valid =
        (candidate_validity_bits.Load((index / 32u) * 4u) & (1u << (index & 31u))) != 0u;
    const float candidate_weight = candidate_is_valid
        ? max(candidate_edge_distance[index], 1.0e-6f)
        : 0.0f;
    const bool replace = candidate_weight > prior_weight[index];
    const float weight = replace ? candidate_weight : prior_weight[index];
    selected_rgb[index] = replace ? candidate_rgb[index] : prior_rgb[index];
    selected_weight[index] = weight;
    coverage[index] = weight > 0.0f ? 1u : 0u;
}

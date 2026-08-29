cbuffer FeatherWeightConstants : register(b0)
{
    uint pixel_count;
    uint source_width;
    uint source_height;
};

ByteAddressBuffer candidate_validity_bits : register(t0);
StructuredBuffer<float> candidate_edge_distance : register(t1);
RWStructuredBuffer<float> feather_weight : register(u0);

[numthreads(64, 1, 1)]
void feather_weights(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    const bool valid =
        (candidate_validity_bits.Load((index / 32u) * 4u) & (1u << (index & 31u))) != 0u;
    const float feather_width = max(1.0f, min(source_width, source_height) * 0.08f);
    feather_weight[index] = valid ? max(candidate_edge_distance[index] / feather_width, 1.0e-6f) : 0.0f;
}

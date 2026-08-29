cbuffer PairDeviationConstants : register(b0)
{
    uint sample_count;
    uint padded_count;
};

StructuredBuffer<float> log_ratios : register(t0);
Buffer<uint> retained : register(t1);
StructuredBuffer<uint4> summary : register(t2);
RWStructuredBuffer<float> deviations : register(u0);

[numthreads(64, 1, 1)]
void prepare_exposure_pair_deviations(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= padded_count)
        return;
    const float median = asfloat(summary[0].x);
    deviations[index] = index < sample_count && retained[index] != 0u
        ? abs(log_ratios[index] - median) : asfloat(0x7F800000u);
}

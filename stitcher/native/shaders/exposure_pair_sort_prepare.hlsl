cbuffer ExposurePairSortPrepareConstants : register(b0)
{
    uint sample_count;
    uint padded_count;
};

StructuredBuffer<float> log_ratios : register(t0);
Buffer<uint> accepted : register(t1);
RWStructuredBuffer<float> sortable_ratios : register(u0);

[numthreads(64, 1, 1)]
void prepare_exposure_pair_sort(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= padded_count)
        return;
    sortable_ratios[index] = index < sample_count && accepted[index] != 0u
        ? log_ratios[index] : asfloat(0x7F800000u);
}

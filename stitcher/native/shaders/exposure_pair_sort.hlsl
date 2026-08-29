cbuffer ExposurePairSortConstants : register(b0)
{
    uint padded_count;
    uint sequence_size;
    uint comparison_stride;
};

RWStructuredBuffer<float> sortable_ratios : register(u0);

[numthreads(64, 1, 1)]
void sort_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= padded_count)
        return;
    const uint partner = index ^ comparison_stride;
    if (partner <= index || partner >= padded_count)
        return;
    const float first = sortable_ratios[index];
    const float second = sortable_ratios[partner];
    const bool ascending = (index & sequence_size) == 0u;
    if ((ascending && first > second) || (!ascending && first < second))
    {
        sortable_ratios[index] = second;
        sortable_ratios[partner] = first;
    }
}

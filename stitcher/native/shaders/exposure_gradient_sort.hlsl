cbuffer ExposureGradientSortConstants : register(b0)
{
    uint padded_count;
    uint sequence_size;
    uint comparison_stride;
};

RWStructuredBuffer<float> sortable_gradients : register(u0);

[numthreads(64, 1, 1)]
void sort_exposure_gradients(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= 2u * padded_count)
        return;
    const uint channel_start = index / padded_count * padded_count;
    const uint local_index = index - channel_start;
    const uint partner = local_index ^ comparison_stride;
    if (partner <= local_index || partner >= padded_count)
        return;
    const uint partner_index = channel_start + partner;
    const float first = sortable_gradients[index];
    const float second = sortable_gradients[partner_index];
    const bool ascending = (local_index & sequence_size) == 0u;
    if ((ascending && first > second) || (!ascending && first < second))
    {
        sortable_gradients[index] = second;
        sortable_gradients[partner_index] = first;
    }
}

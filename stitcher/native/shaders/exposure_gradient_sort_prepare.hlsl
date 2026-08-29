cbuffer ExposureGradientPrepareConstants : register(b0)
{
    uint sample_count;
    uint padded_count;
    uint unused_prepare;
};

StructuredBuffer<float2> gradients : register(t0);
RWStructuredBuffer<float> sortable_gradients : register(u0);

[numthreads(64, 1, 1)]
void prepare_exposure_gradient_sort(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= 2u * padded_count)
        return;
    const uint channel = index / padded_count;
    const uint sample = index % padded_count;
    const float value = sample < sample_count ? gradients[sample][channel] : asfloat(0x7F800000u);
    sortable_gradients[index] = sample < sample_count && isfinite(value)
        ? value : asfloat(0x7F800000u);
}

cbuffer ExposurePairRatioConstants : register(b0)
{
    uint sample_count;
};

StructuredBuffer<float2> pair_luminance : register(t0);
Buffer<uint> accepted : register(t1);
RWStructuredBuffer<float> log_ratios : register(u0);

[numthreads(64, 1, 1)]
void build_exposure_pair_ratios(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    const float2 luminance = pair_luminance[index];
    log_ratios[index] = accepted[index] != 0u && isfinite(luminance.x) && isfinite(luminance.y) &&
        luminance.x > 1.0e-5f && luminance.y > 1.0e-5f ? log(luminance.x / luminance.y) : 0.0f;
}

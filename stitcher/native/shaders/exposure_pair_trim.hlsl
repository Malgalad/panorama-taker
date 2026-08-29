cbuffer ExposurePairTrimConstants : register(b0)
{
    uint sample_count;
};

StructuredBuffer<float> log_ratios : register(t0);
Buffer<uint> accepted : register(t1);
StructuredBuffer<float2> trim_bounds : register(t2);
RWStructuredBuffer<uint> trimmed : register(u0);

[numthreads(64, 1, 1)]
void trim_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    const float ratio = log_ratios[index];
    const float2 bounds = trim_bounds[0];
    trimmed[index] = accepted[index] != 0u && isfinite(ratio) && isfinite(bounds.x) &&
        isfinite(bounds.y) && ratio >= bounds.x && ratio <= bounds.y ? 1u : 0u;
}

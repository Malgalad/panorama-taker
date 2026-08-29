StructuredBuffer<float> sorted_deviations : register(t0);
StructuredBuffer<uint4> summary : register(t1);
RWStructuredBuffer<uint4> result : register(u0);

[numthreads(1, 1, 1)]
void finish_exposure_pair_reduction(uint3 thread_id : SV_DispatchThreadID)
{
    const float difference = asfloat(summary[0].x);
    const uint valid_count = summary[0].y;
    const uint inlier_count = summary[0].z;
    float mad = asfloat(0x7FC00000u);
    if (inlier_count != 0u)
    {
        const uint span = inlier_count - 1u;
        mad = 0.5f * (sorted_deviations[span / 2u] + sorted_deviations[(span + 1u) / 2u]);
    }
    uint reason = 0u;
    if (valid_count < 24u)
        reason = 1u;
    else if (inlier_count < 12u)
        reason = 2u;
    else if (!isfinite(difference) || !isfinite(mad))
        reason = 3u;
    else if (mad > 0.5f)
        reason = 4u;
    const float weight = reason == 0u ? sqrt(float(inlier_count)) / (1.0f + mad) : 0.0f;
    result[0] = uint4(reason, valid_count, inlier_count, asuint(difference));
    result[1] = uint4(asuint(mad), asuint(weight), summary[1].x, 0u);
}

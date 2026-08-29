cbuffer ExposurePairFilterConstants : register(b0)
{
    uint sample_count;
};

StructuredBuffer<float2> gradients : register(t0);
Buffer<uint> categories : register(t1);
StructuredBuffer<float2> gradient_limits : register(t2);
RWStructuredBuffer<uint> accepted : register(u0);

[numthreads(64, 1, 1)]
void filter_resident_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    const float2 gradient = gradients[index];
    const float2 limits = gradient_limits[0];
    accepted[index] = categories[index] != 0u && isfinite(gradient.x) && isfinite(gradient.y) &&
        gradient.x <= limits.x && gradient.y <= limits.y ? 1u : 0u;
}

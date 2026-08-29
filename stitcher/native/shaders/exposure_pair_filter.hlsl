cbuffer ExposurePairFilterConstants : register(b0)
{
    uint sample_count;
    float first_gradient_limit;
    float second_gradient_limit;
};

StructuredBuffer<float2> gradients : register(t0);
Buffer<uint> categories : register(t1);
RWStructuredBuffer<uint> accepted : register(u0);

[numthreads(64, 1, 1)]
void filter_exposure_pair(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= sample_count)
        return;
    const float2 gradient = gradients[index];
    accepted[index] = categories[index] != 0u && isfinite(gradient.x) && isfinite(gradient.y) &&
        gradient.x <= first_gradient_limit && gradient.y <= second_gradient_limit ? 1u : 0u;
}

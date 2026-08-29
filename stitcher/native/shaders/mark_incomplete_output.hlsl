cbuffer MarkIncompleteOutputConstants : register(b0)
{
    uint pixel_count;
};

Buffer<uint> coverage : register(t0);
RWStructuredBuffer<float3> linear_rgb : register(u0);

[numthreads(64, 1, 1)]
void mark_incomplete_output(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    if (coverage[index] == 0u)
        linear_rgb[index] = float3(1.0f, 0.0f, 1.0f);
}

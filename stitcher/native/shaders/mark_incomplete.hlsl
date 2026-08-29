cbuffer MarkIncompleteConstants : register(b0)
{
    uint pixel_count;
};

StructuredBuffer<float3> selected_rgb : register(t0);
StructuredBuffer<float> selected_weight : register(t1);
RWStructuredBuffer<float3> marked_rgb : register(u0);

[numthreads(64, 1, 1)]
void mark_incomplete(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= pixel_count)
        return;
    marked_rgb[index] = selected_weight[index] == 0.0f
        ? float3(1.0f, 0.0f, 1.0f)
        : selected_rgb[index];
}

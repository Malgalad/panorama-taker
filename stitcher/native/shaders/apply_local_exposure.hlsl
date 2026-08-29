cbuffer LocalExposureSampleConstants : register(b0)
{
    uint output_width;
    uint output_height;
    uint row_start;
    uint row_count;
    uint field_width;
    uint field_height;
    uint padding0;
    uint padding1;
};

StructuredBuffer<float3> candidate_rgb : register(t0);
StructuredBuffer<float> local_field : register(t1);
RWStructuredBuffer<float3> adjusted_rgb : register(u0);

[numthreads(64, 1, 1)]
void apply_local_exposure(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    const uint pixel_count = output_width * row_count;
    if (index >= pixel_count)
        return;
    const uint x = index % output_width;
    const uint y = row_start + index / output_width;
    const float field_x = clamp(((x + 0.5f) * field_width / output_width) - 0.5f, 0.0f, field_width - 1.0f);
    const float field_y = clamp(((y + 0.5f) * field_height / output_height) - 0.5f, 0.0f, field_height - 1.0f);
    const uint x0 = (uint)floor(field_x), y0 = (uint)floor(field_y);
    const uint x1 = min(x0 + 1u, field_width - 1u), y1 = min(y0 + 1u, field_height - 1u);
    const float upper = lerp(local_field[y0 * field_width + x0], local_field[y0 * field_width + x1], frac(field_x));
    const float lower = lerp(local_field[y1 * field_width + x0], local_field[y1 * field_width + x1], frac(field_x));
    adjusted_rgb[index] = candidate_rgb[index] * exp(lerp(upper, lower, frac(field_y)));
}

cbuffer Parameters : register(b0)
{
    uint preview_width;
    uint overview_width;
    uint overview_height;
    uint output_width;
    uint output_height;
    uint crop_left;
    uint crop_top;
    uint crop_width;
    uint crop_height;
    uint use_overview;
};

Buffer<uint> preview_rgb8 : register(t0);
Buffer<uint> overview_rgb8 : register(t1);
RWTexture2D<float4> output_rgba8 : register(u0);

[numthreads(8, 8, 1)]
void present_preview_base(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x >= output_width || thread_id.y >= output_height)
        return;
    const uint source_width = use_overview != 0 ? overview_width : crop_width;
    const uint source_height = use_overview != 0 ? overview_height : crop_height;
    const uint x = min(source_width - 1, thread_id.x * source_width / output_width);
    const uint y = min(source_height - 1, thread_id.y * source_height / output_height);
    const uint source_pixel = use_overview != 0
        ? y * overview_width + x
        : (crop_top + y) * preview_width + crop_left + x;
    const uint source_base = source_pixel * 3;
    output_rgba8[thread_id.xy] = float4(
        use_overview != 0 ? overview_rgb8[source_base] : preview_rgb8[source_base],
        use_overview != 0 ? overview_rgb8[source_base + 1] : preview_rgb8[source_base + 1],
        use_overview != 0 ? overview_rgb8[source_base + 2] : preview_rgb8[source_base + 2],
        255.0) / 255.0;
}

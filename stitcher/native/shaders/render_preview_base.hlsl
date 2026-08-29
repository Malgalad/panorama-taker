cbuffer Parameters : register(b0)
{
    uint preview_width;
    uint overview_width;
    uint output_width;
    uint output_height;
    uint crop_left;
    uint crop_top;
    uint use_overview;
};

Buffer<uint> preview_rgb8 : register(t0);
Buffer<uint> overview_rgb8 : register(t1);
RWBuffer<uint> output_rgb8 : register(u0);

[numthreads(64, 1, 1)]
void render_preview_base(uint3 thread_id : SV_DispatchThreadID)
{
    const uint pixel_count = output_width * output_height;
    if (thread_id.x >= pixel_count)
        return;
    const uint x = thread_id.x % output_width;
    const uint y = thread_id.x / output_width;
    const uint source_pixel = use_overview != 0
        ? y * overview_width + x
        : (crop_top + y) * preview_width + crop_left + x;
    const uint output_base = thread_id.x * 3;
    const uint source_base = source_pixel * 3;
    output_rgb8[output_base] =
        use_overview != 0 ? overview_rgb8[source_base] : preview_rgb8[source_base];
    output_rgb8[output_base + 1] =
        use_overview != 0 ? overview_rgb8[source_base + 1] : preview_rgb8[source_base + 1];
    output_rgb8[output_base + 2] =
        use_overview != 0 ? overview_rgb8[source_base + 2] : preview_rgb8[source_base + 2];
}

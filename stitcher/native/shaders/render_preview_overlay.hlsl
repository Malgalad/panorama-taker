cbuffer Parameters : register(b0)
{
    uint preview_width;
    uint preview_height;
    uint overview_width;
    uint overview_height;
    uint mask_width;
    uint mask_height;
    uint crop_left;
    uint crop_top;
    uint frame_count;
    int target_pose;
    uint target_mode;
    uint show_boundaries;
    uint use_overview;
};

Buffer<uint> preview_rgb8 : register(t0);
Buffer<uint> overview_rgb8 : register(t1);
Buffer<uint> compact_masks : register(t2);
Buffer<uint> hovered : register(t3);
RWBuffer<uint> output_rgb8 : register(u0);

bool covered_by_frame(uint frame, uint x, uint y)
{
    const uint mask_x = min(mask_width - 1, x * mask_width / preview_width);
    const uint mask_y = min(mask_height - 1, y * mask_height / preview_height);
    return compact_masks[(frame * mask_height + mask_y) * mask_width + mask_x] != 0;
}

[numthreads(64, 1, 1)]
void render_preview_overlay(uint3 thread_id : SV_DispatchThreadID)
{
    const uint output_width = overview_width;
    const uint output_height = overview_height;
    const uint pixels = output_width * output_height;
    if (thread_id.x >= pixels)
        return;
    const uint x = thread_id.x % output_width;
    const uint y = thread_id.x / output_width;
    const uint source_x = use_overview != 0
        ? min(preview_width - 1, (uint)((x + 0.5) * preview_width / output_width))
        : crop_left + x;
    const uint source_y = use_overview != 0
        ? min(preview_height - 1, (uint)((y + 0.5) * preview_height / output_height))
        : crop_top + y;
    const uint source_base = use_overview != 0
        ? (y * overview_width + x) * 3
        : (source_y * preview_width + source_x) * 3;
    float3 color = use_overview != 0
        ? float3(overview_rgb8[source_base], overview_rgb8[source_base + 1], overview_rgb8[source_base + 2])
        : float3(preview_rgb8[source_base], preview_rgb8[source_base + 1], preview_rgb8[source_base + 2]);
    const uint left_output_x = x == 0 ? output_width - 1 : x - 1;
    const uint right_output_x = x == output_width - 1 ? 0 : x + 1;
    const uint left_x = use_overview != 0
        ? min(preview_width - 1, (uint)((left_output_x + 0.5) * preview_width / output_width))
        : crop_left + left_output_x;
    const uint right_x = use_overview != 0
        ? min(preview_width - 1, (uint)((right_output_x + 0.5) * preview_width / output_width))
        : crop_left + right_output_x;
    const uint top_output_y = y == 0 ? 0 : y - 1;
    const uint bottom_output_y = min(y + 1, output_height - 1);
    const uint top_y = use_overview != 0
        ? min(preview_height - 1, (uint)((top_output_y + 0.5) * preview_height / output_height))
        : crop_top + top_output_y;
    const uint bottom_y = use_overview != 0
        ? min(preview_height - 1, (uint)((bottom_output_y + 0.5) * preview_height / output_height))
        : crop_top + bottom_output_y;
    for (uint frame = 0; frame < frame_count; ++frame)
    {
        if (!covered_by_frame(frame, source_x, source_y))
            continue;
        if (hovered[frame] != 0)
        {
            const float3 tint = target_mode != 0 ? float3(0, 102, 255) : float3(255, 0, 255);
            color = color * 0.8 + tint * 0.2;
        }
        const bool boundary = !covered_by_frame(frame, left_x, source_y) ||
            !covered_by_frame(frame, right_x, source_y) ||
            !covered_by_frame(frame, source_x, top_y) ||
            !covered_by_frame(frame, source_x, bottom_y);
        if ((show_boundaries != 0 || hovered[frame] != 0) && boundary)
            color = (int)frame == target_pose ? float3(0, 102, 255) : float3(255, 0, 255);
    }
    const uint output_base = thread_id.x * 3;
    output_rgb8[output_base] = (uint)color.r;
    output_rgb8[output_base + 1] = (uint)color.g;
    output_rgb8[output_base + 2] = (uint)color.b;
}

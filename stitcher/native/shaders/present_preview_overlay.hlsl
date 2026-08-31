cbuffer Parameters : register(b0)
{
    uint preview_width;
    uint preview_height;
    uint overview_width;
    uint overview_height;
    uint mask_width;
    uint mask_height;
    uint output_width;
    uint output_height;
    uint crop_left;
    uint crop_top;
    uint crop_width;
    uint crop_height;
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
RWTexture2D<float4> output_rgba8 : register(u0);

uint2 source_position(uint output_x, uint output_y)
{
    return use_overview != 0
        ? uint2(
            min(preview_width - 1, (uint)((output_x + 0.5) * preview_width / output_width)),
            min(preview_height - 1, (uint)((output_y + 0.5) * preview_height / output_height)))
        : uint2(
            crop_left + min(crop_width - 1, output_x * crop_width / output_width),
            crop_top + min(crop_height - 1, output_y * crop_height / output_height));
}

bool covered_by_frame(uint frame, uint2 source)
{
    const uint mask_x = min(mask_width - 1, source.x * mask_width / preview_width);
    const uint mask_y = min(mask_height - 1, source.y * mask_height / preview_height);
    return compact_masks[(frame * mask_height + mask_y) * mask_width + mask_x] != 0;
}

[numthreads(8, 8, 1)]
void present_preview_overlay(uint3 thread_id : SV_DispatchThreadID)
{
    if (thread_id.x >= output_width || thread_id.y >= output_height)
        return;
    const uint2 source = source_position(thread_id.x, thread_id.y);
    const uint overview_x = min(overview_width - 1, thread_id.x * overview_width / output_width);
    const uint overview_y = min(overview_height - 1, thread_id.y * overview_height / output_height);
    const uint source_base = use_overview != 0
        ? (overview_y * overview_width + overview_x) * 3
        : (source.y * preview_width + source.x) * 3;
    float3 color = use_overview != 0
        ? float3(overview_rgb8[source_base], overview_rgb8[source_base + 1], overview_rgb8[source_base + 2])
        : float3(preview_rgb8[source_base], preview_rgb8[source_base + 1], preview_rgb8[source_base + 2]);
    const uint left_x = thread_id.x == 0 ? output_width - 1 : thread_id.x - 1;
    const uint right_x = thread_id.x == output_width - 1 ? 0 : thread_id.x + 1;
    const uint top_y = thread_id.y == 0 ? 0 : thread_id.y - 1;
    const uint bottom_y = min(thread_id.y + 1, output_height - 1);
    const uint2 left = source_position(left_x, thread_id.y);
    const uint2 right = source_position(right_x, thread_id.y);
    const uint2 top = source_position(thread_id.x, top_y);
    const uint2 bottom = source_position(thread_id.x, bottom_y);
    for (uint frame = 0; frame < frame_count; ++frame)
    {
        if (!covered_by_frame(frame, source))
            continue;
        if (hovered[frame] != 0)
        {
            const bool reference = (int)frame == target_pose;
            const float3 tint = target_mode != 0 || reference
                ? float3(0, 102, 255)
                : float3(255, 0, 255);
            color = color * 0.8 + tint * 0.2;
        }
        const bool boundary = !covered_by_frame(frame, left) ||
            !covered_by_frame(frame, right) || !covered_by_frame(frame, top) ||
            !covered_by_frame(frame, bottom);
        if ((show_boundaries != 0 || hovered[frame] != 0) && boundary)
            color = (int)frame == target_pose ? float3(0, 102, 255) : float3(255, 0, 255);
    }
    output_rgba8[thread_id.xy] = float4(color, 255.0) / 255.0;
}

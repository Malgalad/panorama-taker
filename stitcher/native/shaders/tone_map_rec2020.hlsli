float3 tone_map_rec2020(float3 linear_rec2020, float reference_white_nits)
{
    const float3 relative = max(linear_rec2020, 0.0) * (10000.0 / reference_white_nits);
    const float luminance =
        relative.r * 0.2627 + relative.g * 0.6780 + relative.b * 0.0593;
    const float mapped_luminance = luminance / (1.0 + luminance);
    const float scale = luminance > 0.0 ? mapped_luminance / luminance : 0.0;
    return relative * scale;
}

float linear_channel_to_normalized_srgb(float value)
{
    const float nonnegative = max(value, 0.0f);
    const float encoded = nonnegative <= 0.0031308f
        ? nonnegative * 12.92f
        : 1.055f * pow(nonnegative, 1.0f / 2.4f) - 0.055f;
    return saturate(encoded);
}

float3 linear_to_normalized_srgb(float3 value)
{
    return float3(
        linear_channel_to_normalized_srgb(value.x),
        linear_channel_to_normalized_srgb(value.y),
        linear_channel_to_normalized_srgb(value.z));
}

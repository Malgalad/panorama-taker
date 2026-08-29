float decode_source_transfer_channel(float value, uint transfer_code)
{
    float decoded = value;
    if (transfer_code == 1u)
    {
        if (value <= 0.04045f)
            decoded = value / 12.92f;
        else
            decoded = pow(max((value + 0.055f) / 1.055f, 0.0f), 2.4f);
    }
    else if (transfer_code == 2u)
    {
        const float powered = pow(max(value, 0.0f), 32.0f / 2523.0f);
        decoded = pow(max((powered - 3424.0f / 4096.0f) /
            max(2413.0f / 128.0f - 2392.0f / 128.0f * powered, 1.17549435e-38f), 0.0f),
            16384.0f / 2610.0f);
    }
    return decoded;
}

float3 decode_source_transfer(float3 value, uint transfer_code)
{
    return float3(
        decode_source_transfer_channel(value.x, transfer_code),
        decode_source_transfer_channel(value.y, transfer_code),
        decode_source_transfer_channel(value.z, transfer_code));
}

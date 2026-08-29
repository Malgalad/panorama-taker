cbuffer QuantizeSrgb8Constants : register(b0)
{
    uint channel_count;
};

StructuredBuffer<float> normalized_srgb : register(t0);
RWBuffer<uint> quantized_srgb : register(u0);

[numthreads(64, 1, 1)]
void quantize_srgb8(uint3 thread_id : SV_DispatchThreadID)
{
    const uint index = thread_id.x;
    if (index >= channel_count)
        return;
    const float scaled = saturate(normalized_srgb[index]) * 255.0f;
    const float lower_float = floor(scaled);
    uint value = (uint)lower_float;
    const float fraction = scaled - lower_float;
    if (fraction > 0.5f || (fraction == 0.5f && (value & 1u) != 0u))
        ++value;
    quantized_srgb[index] = value;
}

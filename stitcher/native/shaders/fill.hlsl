RWStructuredBuffer<uint> output : register(u0);

[numthreads(16, 1, 1)]
void fill(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    output[dispatch_thread_id.x] = dispatch_thread_id.x * 3 + 1;
}

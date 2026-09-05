# Native D3D12 failure cleanup

`wait_for_fence` treats its 10-second timeout as an operation failure, not proof that
the GPU has stopped. On timeout, event-creation failure, or event-registration/wait
failure, it keeps the caller's stack-owned resources, descriptors, command list,
and allocator alive until the fence completes or the device is removed. A registered
event handle also remains alive through this drain. The original failure is still
returned, so a failed readback never publishes potentially invalid bytes.

`signal_fence` also preserves the original HRESULT on failure. Since commands may
already be submitted, it retries the queue signal and drains that fence before
returning, or exits once device removal is detected. The upload teardown's existing
recovery signal retains its separate retain-on-failure ownership policy.

These recovery paths do not allocate another image buffer, reset the GPU device,
or alter successful rendering. Cleanup can exceed ten seconds; a queue that never
finishes and is never removed can keep its worker blocked indefinitely. Cancellation
does not make in-flight GPU resources safe to destroy. A bounded-return alternative
would require deferred ownership and retirement of every submitted dependency.

The GPU contract test blocks the queue on a separate, test-only fence, injects a
real zero-duration event timeout or a queue-signal failure, and observes the drain
before releasing the gate from the CPU. It verifies that the operation cannot return
early, failed readbacks leave the destination unchanged, and subsequent readback
recovers with correct pixels and transfer counters. No CPU signal is applied to the
production completion fence.

## Microsoft references

- [Command-list ownership and allocator lifetime](https://learn.microsoft.com/en-us/windows/win32/direct3d12/recording-command-lists-and-bundles).
- [Fence completion events and event-registration failures](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion).
- [Queue-side versus CPU-side fence signaling](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-signal).

This documents the failure-lifetime fix, not a certification of every backend path
or AMD/Intel hardware compatibility. Hardware verification on those vendors remains
deferred.

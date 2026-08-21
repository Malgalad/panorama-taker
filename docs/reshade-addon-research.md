# ReShade screenshot add-on research

Last checked: 2026-08-21 against the installed 64-bit ReShade 6.7.3 runtime.

## Decision

Use a small ReShade add-on as the primary screenshot adapter. It can both request a screenshot and observe successful completion without simulating Print Screen or guessing which file appeared.

The official `reshade::api::effect_runtime` interface exposes `save_screenshot(const char *postfix)`, which captures the current back buffer and saves it through ReShade. The official `reshade::addon_event::reshade_screenshot` callback runs after a screenshot was saved and supplies `(effect_runtime *, const char *path)`. The returned path, not the overlay notification or a directory scan, is the authoritative capture result.

The exact callback path also means the adapter does not need to parse `[SCREENSHOT] SavePath`. It may query that setting through ReShade's official `get_config_value` helper for preflight/status, but association must still use the callback result.

Do not use `capture_screenshot(void *pixels)` for production. It copies raw back-buffer bytes into caller-owned memory and would bypass ReShade's file naming, configured destination, and image encoding.

## Minimal add-on shape

1. Vendor the official headers from ReShade tag `v6.7.3`, whose `reshade.hpp` declares API 18, and build a 64-bit DLL named with the `.addon64` suffix. The installed log confirms ReShade 6.7.3.2148 and successful API 18 add-ons; current `main` headers declare API 20 and must not be used for this target.
2. Register the add-on during process attach, then register callbacks for `init_effect_runtime`, `destroy_effect_runtime`, `reshade_present`, and `reshade_screenshot`.
3. Track only live runtime pointers. Clear pending state when its runtime is destroyed or the add-on unloads.
4. Accept one plain-data request: session ID, pose index, and a short filesystem-safe token.
5. Consume that request in `reshade_present` and call `runtime->save_screenshot(token)`. IPC threads must not call the runtime.
6. Complete the request only when `reshade_screenshot` returns the same runtime and a path containing the token.
7. Emit the exact path and request identity to the coordinator. Ignore unrelated manual screenshots.

The add-on callback should only enqueue a compact completion record. File decoding, schema validation, manifest updates, and CET advancement belong outside the render callback.

## Required proof before automation

The current ReShade hotkey produces a 3840×2160 16-bit Rec.2020/PQ PNG with effects. The API documentation establishes that `save_screenshot()` writes an image through ReShade, but it does not promise Cyberpunk-specific HDR equivalence. Capture one add-on-triggered image and verify dimensions, bit depth, color primaries/transfer characteristics, and visible effect inclusion against a hotkey-triggered image.

If that proof fails, keep `reshade_screenshot` for exact completion/path reporting and use ReShade's normal screenshot hotkey as a compatibility trigger. The stable-file watcher remains the last-resort fallback for runtimes without the required add-on event.

## Integration boundary

CET Lua cannot directly invoke a C++ ReShade add-on. Packet 7 should first prove the adapter with an add-on-owned test hotkey or overlay action. Packet 8 should add a bounded local bridge. First test whether CET can use a named pipe without blocking `onUpdate`; otherwise use atomic request and acknowledgement files in a dedicated control directory. That fallback observes tiny IPC records, not screenshot creation, and completion still comes exclusively from `reshade_screenshot`.

State is strictly single-flight:

```text
idle -> request_pending -> save_requested -> awaiting_event -> completed
                                            \-> timed_out/error
```

The camera must remain at the current pose until the matching completion has been validated and committed atomically.

## Primary references

- [ReShade API overview](https://crosire.github.io/reshade-docs/)
- [`effect_runtime::save_screenshot` in v6.7.3](https://github.com/crosire/reshade/blob/v6.7.3/include/reshade_api.hpp)
- [`reshade_screenshot` event and callback signature in v6.7.3](https://github.com/crosire/reshade/blob/v6.7.3/include/reshade_events.hpp)
- [ReShade v6.7.3 runtime screenshot scheduling](https://github.com/crosire/reshade/blob/v6.7.3/source/runtime.cpp)

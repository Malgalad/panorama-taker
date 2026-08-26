# Capture-time exposure pinning research

## Status

This is a future capture-side idea, not an implementation decision. No current
capture behavior, dependency, setting, or restoration contract changes.

Cyberpunk's automatic exposure can change when the panorama camera turns toward
different luminance distributions. The stitcher compensates for resulting
overlap discontinuities, but preventing the changes during capture may preserve
more consistent source pixels.

## Candidate Codeware path

The installed Codeware native declarations expose the pieces needed for a
development probe from CET Lua:

1. `Game.GetWeatherSystem():GetEnvironmentDefinition()` returns the active
   `worldEnvironmentDefinition`.
2. `worldEnvironmentDefinition.worldRenderSettings.areaParameters` contains
   `ref<IAreaSettings>` values. An exposure entry can be identified with
   `setting:IsA("ExposureAreaSettings")`.
3. `ExposureAreaSettings` exposes `exposureMin` and `exposureMax` as
   `CurveDataFloat` values.
4. Codeware exposes `CurveDataFloat.GetSize`, `GetPoint`,
   `GetInterpolationType`, `GetLinkType`, `SetPointValue`, and related mutation
   methods.

A candidate pin would snapshot both complete curves, including every point and
value plus interpolation and link types, then replace every value in both
curves with one chosen EV. Preserving the existing point positions and array
sizes is preferable to resizing the curves. Restoration would write the exact
snapshot back on completion, abort, failure, and CET shutdown.

This mutates a live shared resource. It must not be attempted in the production
capture path until an isolated in-game probe proves that the renderer consumes
the mutations safely and immediately.

## What this does not provide

The exposed curves describe the allowed exposure range over time; they do not
report the renderer's currently adapted exposure. No verified RED4ext,
Codeware, CET, camera-system, or weather-system method was found for reading
that live value.

Consequently, the candidate path can:

- lock exposure to an explicitly selected EV;
- restore the original min/max configuration exactly; and
- allow normal auto-exposure to resume after restoration.

It cannot currently:

- lock seamlessly to the exact exposure visible at capture start;
- restore the renderer's internal adaptation history; or
- guarantee that the post-restore image will not visibly readapt.

Reading the live adapted value likely requires a RED4ext native renderer hook
or access to the GPU exposure/adaptation resource. That is a materially wider
native investigation and is not justified before the curve-mutation probe.

## Unresolved engine behavior

The global environment definition may not be the only effective source of
exposure settings. Weather-state environment resources and local environment
area notifiers can contribute or override `ExposureAreaSettings`. Mutating only
`worldRenderSettings` may therefore work in some locations and fail in others.
The probe must test interiors, exteriors, transitions, and at least one strong
local exposure volume.

It is also unknown whether changing the serialized curve object invalidates or
refreshes the renderer's processed environment state. A successful RTTI write
does not by itself prove a visual change.

## Proposed development probe

Before any production integration:

1. Add a development-only binding that locates and logs all reachable
   `ExposureAreaSettings`, including curve topology and values.
2. Snapshot the curves without changing them and verify lossless round-trip
   restoration.
3. With an explicit test EV, set every min/max curve value to that EV and check
   whether view rotation stops exposure changes.
4. Restore on a second binding and on every lifecycle exit, then verify the
   original curve data byte-for-value at the exposed field level.
5. Repeat across representative exterior, interior, weather, and local-volume
   scenes, in SDR and HDR capture modes.
6. Reject the approach if it needs resource-cache invalidation, leaves shared
   weather data modified, or cannot restore reliably after CET reload.

Even if the probe succeeds, stitcher exposure compensation should remain the
fallback because scripted scenes, local overrides, or future game updates may
bypass the pin.

## Sources inspected

- Installed Codeware generated declarations in
  `red4ext/plugins/Codeware/Scripts/Codeware.Global.reds`.
- Codeware resource-reference API:
  <https://github.com/psiberx/cp2077-codeware/blob/main/scripts/Depot/ResourceReference.reds>.
- RED4ext resource loader API:
  <https://github.com/WopsS/RED4ext.SDK/blob/master/include/RED4ext/ResourceLoader.hpp>.
- Codeware releases, including recent curve mutation fixes:
  <https://github.com/psiberx/cp2077-codeware/releases>.

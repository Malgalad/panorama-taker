# CET FPP capture-environment references

Last checked against the installed mods on 2026-08-20. These are behavioral and API references for an independent implementation; do not copy third-party source without checking its license.

## Freeze world time while retaining camera control

### Appearance Menu Mod 2.12.5

Installed source: `AppearanceMenuMod/Modules/tools.lua`, functions `Tools:SetSlowMotionSpeed` and `Tools:FreezeTime`.

AMM demonstrates two relevant approaches:

- call `Game.GetTimeSystem():SetIgnoreTimeDilationOnLocalPlayerZero(true)` and apply a very small nonzero dilation (`1e-13`) with `SetTimeDilation`;
- apply true zero dilation under the `pause` reason and use `TimeDilationHelper` to exempt the local player.

Use the near-zero approach for the first PanoramaCapture feasibility probe because it has fewer coupled helper calls. Production code must use a project-specific reason such as `PanoramaCapture` and remove only that reason. Do not reuse AMM's `consoleCommand` reason because another mod may own it.

The inspected CET examples expose `IsTimeDilationActive(reason)` but no getter for the global `SetIgnoreTimeDilationOnLocalPlayerZero` flag. Therefore the feasibility probe requires AMM, FreeFly, WeatherSwitcher, and other time-control features to be inactive, sets the flag true on entry, and restores it to false on exit. Production must either prove that the `TimeDilationHelper` player-scoped path composes safely or reject capture when a known conflicting dilation reason is active; it must not claim to restore an unreadable pre-existing global flag.

### FreeFly and WeatherSwitcher

- `freefly/modules/utils/logic.lua` applies `SetTimeDilation("console", 1e-9)` and removes it with `UnsetTimeDilation("console")`.
- `WeatherSwitcher/Modules/TimeController.lua` combines `SetIgnoreTimeDilationOnLocalPlayerZero(scale ~= 1)` with a `consoleCommand` time-dilation reason.

Together with AMM, these provide independent evidence that CET Lua can keep the player/camera responsive while slowing the world effectively to a stop. They also illustrate why PanoramaCapture needs its own reason and must not globally undo another mod's dilation.

## Hide the player renderer

### Appearance Menu Mod “Invisible V”

Installed source: `AppearanceMenuMod/Modules/tools.lua`, function `Tools:ToggleInvisibleBody`.

AMM enumerates `Game.GetPlayer():GetComponents()`, selects components whose class name contains `Mesh`, and calls `comp:Toggle(false)` while Invisible V is active. This is the correct basis for capture-time visual hiding. AMM's separate `Tools:ToggleInvisibility` calls `PlayerPuppet:SetInvisible` and `UpdateVisibility`; that is gameplay/passive-mode behavior and is not the preferred capture mechanism.

PanoramaCapture must strengthen the mesh approach:

1. Record each matching component handle and its exact `comp:IsEnabled()` value.
2. Disable only enabled mesh components.
3. Re-scan at capture transitions and track any newly created mesh component before disabling it.
4. Restore each recorded component to its original enabled state, rather than enabling every mesh.
5. Reject equipment changes during capture and abort safely if the player puppet is replaced or detached.

The active weapon is a separate entity and is not guaranteed to appear in `PlayerPuppet:GetComponents()`. AMM's `Util:GetPlayerWeapon` uses `EquipmentSystem:GetActiveWeaponObject(Game.GetPlayer(), 40)`; PanoramaCapture uses that same observed API only to include the active weapon's mesh components in its own reversible capture transaction.

## Lock player and camera input during capture

### Appearance Menu Mod player effects

Installed source: `AppearanceMenuMod/Modules/util.lua`, functions `Util:AddPlayerEffects` and `Util:RemovePlayerEffects`.

AMM applies `GameplayRestriction.NoMovement` and `GameplayRestriction.NoCameraControl` through `Game.GetStatusEffectSystem():ApplyStatusEffect`, then removes them with `RemoveStatusEffect` when its transaction ends. PanoramaCapture uses those two effects while a capture session is active, so an accidental movement or mouse/controller bump cannot alter a pose between readiness and the manual screenshot.

This is a known composition limitation: CET does not expose a trustworthy owner/stack query for these effects, and removal can therefore clear an identical restriction applied by another mod. Do not use FreeFly concurrently with PanoramaCapture; observed testing shows the controllers conflict. Preserve this limitation in release documentation until the engine exposes ownership-aware effect removal.

The in-game feasibility test must check body, head, arms, weapon, clothing, cyberware, shadows, and reflections at representative yaw and polar pitch poses. Component toggling is promising source-level evidence, not proof that every render proxy disappears until this visual test passes.

## Hide HUD and transient UI

### WindowSwitcher

Installed source: `WindowSwitcher/init.lua`, function `setHUDVisible`.

WindowSwitcher gets `Game.GetInkSystem():GetLayer(CName.new("inkHUDLayer")):GetVirtualWindow()`, enumerates its children, and sets their opacity to zero. PanoramaCapture should record each child's original opacity, hide it, and restore the saved value. It should re-scan at capture transitions so a newly created prompt or marker cannot appear in a later frame.

### HUDToggler and CET game settings

The community [HUDToggler](https://github.com/chaotic-developments-cp2077/HUDToggler) demonstrates a broader settings-based fallback covering individual `/interface/hud` preferences. Treat its GPL-licensed source as a behavior/API reference, not code to copy. The ink layer remains the primary reversible mechanism because changing user settings has a larger persistence surface.

The feasibility test must include crosshair, minimap, quest tracker, interaction prompts, world markers, damage overlays, notifications, and widgets created after capture begins. UI outside `inkHUDLayer` must be identified and handled explicitly or documented as an incompatible capture state.

## Measure real FPS and detect frame generation

### Ultra+ and UltraTool

Installed sources:

- `UltraPlus/lib/Functions.lua`, function `UpdateStats`;
- `UltraPlus/lib/Cyberpunk.lua`, function `Cyberpunk.GetOption`;
- `UltraPlus/lib/GameMenu.lua`, graphics-setting inventory;
- `UltraPlus/lib/Engine.lua`, function `Engine.CheckUltraToolAvailable`.

Ultra+ calculates real game FPS from CET's `onUpdate(deltaTime)` as `1 / deltaTime` and smooths it with an exponential moving average. When its optional RED4ext UltraTool plugin is loaded, `GetFrameCount()` supplies swap-chain present counts, allowing Ultra+ to calculate a separate frame-generated/presented FPS. PanoramaCapture does not need the presented count for temporal settling, so UltraTool must remain optional.

Ultra+ reads graphics options through `Game.GetSettingsSystem():GetVar(group, name)`. PanoramaCapture should inspect all applicable `/graphics/presets` settings rather than only the legacy DLSS boolean:

- `DLSSFrameGen`;
- `DLSS_MultiFrameGeneration`;
- `FSR3_FrameGeneration`;
- `XESS_FrameGeneration`;
- `FrameGeneration`.

Each lookup must be guarded because availability and value type depend on the game version, selected upscaler, and hardware. Record the raw available values plus a normalized `enabled` result in session metadata. If a native present counter is available, record presented FPS and the presented-to-real ratio as diagnostics, but never use generated presents to satisfy temporal settling.

After the final camera write for a pose, count CET `onUpdate` callbacks and accumulate elapsed update time. A screenshot becomes eligible only after at least the configured number of real game frames (default 8) and the configured stabilization floor (currently 1.5 seconds for observed global-illumination settling) have both passed. FPS is used for status, estimated wait time, metadata, and a generous watchdog timeout—not to replace either condition with an unbounded wall-clock sleep.

## Selected Packet 4 strategy

One two-click CET probe will exercise the complete environment transaction:

1. Verify there is no conflicting time-control session, then snapshot FPP pose/FoV, HUD child opacities, and player mesh enabled states.
2. Apply PanoramaCapture near-zero dilation while exempting the local player.
3. Hide HUD children and player mesh components.
4. Apply one representative absolute yaw/pitch pose and wait only for deterministic frame settling.
5. On the second click, restore pose, mesh states, HUD opacities, local-player dilation behavior, and the PanoramaCapture dilation reason.

RED4ext remains a fallback only if this measured CET probe fails a specific requirement.

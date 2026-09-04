# Time-freeze animation and pause-menu research

## Status

Investigation stopped on 2026-09-05 without a production change. Freezing gameplay time is not
enough to stop every visible animation, while the complete pause-menu path stops the live scene
render needed for panorama capture. The temporary CET and RED4ext probes described below are not
part of the production mod.

## Observed behavior

- FreeFly time freeze stops ordinary gameplay motion, but animated adverts and color-changing
  light panels can continue.
- A bare `PauseGame`/`UnpauseGame` test is not equivalent to opening the pause menu. It left INK
  animation running and produced severe DLSS wobble on otherwise stationary NPCs, unlike AMM's
  time freeze.
- Opening the real pause menu with `Esc` stopped the tested adverts, light panels, and audio.
- Hiding pause-menu UI did not remove the compositor blur or cached background frame.
- On leaving the pause menu, an advert could reappear at a different animation time. This is
  consistent with a clock accumulating while its output is not rendered, or with animation state
  being reevaluated on resume; the test did not distinguish those cases.

## Advertisement and INK enumeration

Runtime INK-layer enumeration did not locate the visible advert population:

- `inkAdvertisementsLayer` had no controllers or children in the tested scene.
- `inkWorldLayer` exposed a small set of vending-machine and terminal controllers, far fewer than
  the animated adverts around the player.
- `inkOffscreenLayer` exposed global-TV controllers, but not the nearby world adverts.
- The advertisement layer's animation processor had no active entries in the inspected
  containers.
- Candidate layer arrays contained resource hashes and associated objects rather than an obvious
  collection of live `inkanimProxy` instances.

RTTI confirmed that `inkanimProxy` has native `Pause`, `Resume`, `Stop`, `IsPlaying`, `IsPaused`,
and `GetTargets` methods. The missing piece is a reliable way to enumerate the proxies responsible
for world adverts. Attempts to observe `inkWidget.PlayAnimation` and
`inkWidget.PlayAnimationWithOptions` did not capture the visible advert starts, and the native
hook lookup did not resolve those targets.

These results also leave open whether some displays are video, material, shader, or entity-driven
animations rather than INK animations. Searching only INK widgets is therefore not a complete
strategy.

## Pause-menu sequence

Tracing the normal `Esc` path produced this order:

1. `MenuScenario_Idle.OnOpenPauseMenu`
2. switch to `MenuScenario_PauseMenu`
3. `inkMenusState.ShowMenus(true)`
4. open `pause_menu_background`
5. open `pause_menu`
6. `PauseGame`

Leaving the menu switches back to `MenuScenario_Idle`, hides menus, and calls `UnpauseGame`.
Blocking either pause widget or calling `ShowMenus(false)` could remove portions of the UI, but it
did not clear the fullscreen compositor's blur and cached-frame treatment.

## Preem Menu and HUD Painter

The original Preem Menu archive loaded successfully without HUD Painter: its cleaner pause-menu
layout appeared, proving that the archive itself was active. The blur and cached image remained.

HUD Painter's scripts do not control fullscreen composition, pause state, blur, or background
capture. Its pause-menu integration adds a HUD Painter menu item; the rest of the dependency
manages colors, presets, storage, and UI resources. It was therefore not needed to explain the
remaining blur.

Preem's fullscreen composition resource changes `inkPauseMenuState.useBackgroundTexture` from
`1` to `0`, but leaves the vanilla pause shader parameters intact, including:

- `blurredRenderFactor = 0.507937014`
- `mainRenderFactor = 0.5`
- `backgroundBlurRadius = 0.0160000008`
- the pause vignette, glow, shadow, and chromatic-aberration parameters

Its `pause_menu_background.inkwidget` also hides a dark rectangle and vignette widget, while
`pause_menu.inkwidget` contains the broader cosmetic redesign.

## Clean-compositor probe

A custom resource replaced the `inkPauseMenuState` shader parameters with the normal
`inkGameState` values, set `blurredRenderFactor` and `backgroundTextureFactor` to zero, set
`mainRenderFactor` to one, and kept `useBackgroundTexture` disabled. The pause background became
fully black.

This is strong evidence that the complete pause path does not keep a live world render available
to the fullscreen compositor. The normal pause image is supplied through the cached/background
path; removing that path does not reveal a sharp, continuously rendered frozen scene.

## Conclusion

The real pause menu is the only tested path that stopped all targeted animation and audio, but it
cannot currently serve panorama capture because it also removes the live scene render and camera
updates required for new views. Conversely, gameplay time dilation preserves rendering and camera
control but does not cover every advert, video, UI, material, or light animation clock.

If this is revisited, the next useful direction is identifying the actual clock or runtime object
used by each visible display class, then pausing and restoring those systems independently. A
production solution must snapshot and restore every affected object on success, abort, failure,
shutdown, and streaming changes. Broad `PauseGame` use and fullscreen pause-menu composition are
not suitable capture mechanisms based on these tests.

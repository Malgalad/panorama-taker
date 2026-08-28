# Panorama preview implementation plan

> Historical implementation plan. The current code and tests are authoritative; manual exposure
> behavior, pose-overlay interaction, and its resource/threading invariants are documented in
> [exposure-normalization.md](exposure-normalization.md). Later implementation results are recorded
> in [progress.md](progress.md).

## Goal

Change the GUI render flow from one action into two stages:

1. The first action renders and displays an ephemeral panorama preview.
2. After the preview succeeds, the user can either discard it or start the existing full-size
   render.

The preview occupies a new right-hand column. Showing it grows the window horizontally and does
not add UI below the existing left-hand controls. The preview width is fixed when previewing
starts: use the current window's client width minus its existing 12-pixel outer padding on both
sides. Do not recalculate this width after the preview column makes the window wider.

This is a GUI workflow feature. Do not change the CLI, session schema/history, panorama projection,
output naming, selected final dimensions, coverage behavior, or final color pipeline.

## Fixed behavior

Use the following behavior exactly:

- **Abort scope:** `Discard preview` removes only scratch and temporary files made by the current
  preview/render attempt. It does not delete pre-existing panorama, coverage, thumbnail, source,
  or session files.
- **Preview format:** the preview is an 8-bit Tk-displayable sRGB image held in memory after it is
  loaded. It is not published beside the selected output and is never added to session history.
- **EXR display:** previews are SDR even when EXR is selected. Use the existing `_to_sdr_srgb`
  display conversion without auto contrast. This conversion is preview-only; do not change,
  clamp, tone-map, or auto-contrast the final EXR output pixels.
- **Session thumbnail:** `Generate session thumbnail` remains a full-render artifact and is not
  generated during preview.

## State and ownership

Introduce one small compositor-owned preview state object rather than teaching the GUI about
scratch files. A suitable shape is a private or public dataclass containing:

- the validated/renderable session identity and render-affecting option fingerprint;
- the solved `ExposureReport`;
- a `TemporaryDirectory` (or equivalent owned scratch directory) that remains alive between the
  preview and full-render actions;
- the preview's display pixels or temporary preview path; and
- an idempotent `close()` method that closes memmaps/writers first and then removes the entire
  scratch directory.

The option fingerprint must cover session path/identity, source directory, source frame list,
blend, `allow_incomplete`, auto contrast, memory budget, and worker selection. Final-only values
such as output path, output dimensions, output encoding, JPEG quality, coverage, and thumbnail do
not invalidate the exposure solve, but any changed GUI input should conservatively discard the
displayed preview and state so the user cannot render settings they did not preview.

The GUI owns at most one preview state. Close it on discard, input change, preview replacement,
preview failure, full-render success/failure/cancellation, and application shutdown. Cleanup must
be safe to call more than once.

## Compositor refactor

Keep the refactor narrow and preserve `render_session` as the CLI-compatible entry point.

1. Extract the beginning of `render_session` into helpers for validation, source probing,
   renderable-session filtering, and `_estimate_exposure_gains`. Do not alter the exposure solver.
2. Add an optional keyword-only `exposure_report: ExposureReport | None = None` at the end of the
   internal render path. When supplied, validate that its gain count matches the filtered frame
   count and skip `_estimate_exposure_gains`; otherwise solve exposure exactly as today.
3. Leave the public `render_session` behavior unchanged by having it call the internal path with no
   report. Existing GUI, CLI, and tests must remain source-compatible.
4. Add a preview entry point that accepts an explicit preview width and the same relevant render
   options. Validate width as positive and derive height through `_output_dimensions`, preserving
   full-sphere and horizontal-session conventions.
5. The preview entry point first solves `ExposureReport`, stores it in the owned preview state, and
   then renders the equirectangular target at preview dimensions.
6. Reuse the existing direction generation, `_exposure_weight`, local exposure multiplier,
   hard/feather blending, incomplete-coverage checks/magenta behavior, source decoding, and bounded
   strip planning. Do not create a second preview-only compositor algorithm.
7. Compute a new local exposure field for the preview dimensions. Keep it disk-backed and
   strip-based. Never reuse or rescale that field for the full render.
8. Normalize preview feather weights and compute preview auto-contrast levels from preview pixels
   when the selected final output is PNG/JPEG and auto contrast is enabled. Recompute both for the
   final dimensions later.
9. Convert preview rows to display sRGB with the existing conversion helpers, encode them to a
   lossless temporary PNG (or return an owned PIL image), load/copy the image for GUI delivery, and
   remove the encoded temporary immediately. Do not use selected JPEG quality for preview.
10. Check cancellation in every frame/strip/analysis/write loop. On any exception, close all
    memmaps and writers and remove the preview state directory.
11. When full render is requested, pass the stored `ExposureReport` into the internal render path.
    All full-resolution maps, auto-contrast analysis, output `.partial` files, coverage, and
    optional session thumbnail are still produced by the existing final pipeline.
12. Preserve the current delayed publication behavior: final panorama/coverage/thumbnail files
    are replaced only after every requested final artifact is successfully written. Preview must
    never trigger overwrite or publication.

Only the solved relative exposure gains are reusable. This is intentional:

| Work | Preview | Full render | Reuse? |
| --- | --- | --- | --- |
| Source validation/probing | yes | validate fingerprint/state | state metadata only |
| Relative exposure solve | yes | no second solve | yes |
| Local exposure field | preview dimensions | final dimensions | no |
| Composite/weight buffers | preview dimensions | final dimensions | no |
| Auto-contrast levels | preview pixels | final pixels | no |
| Encoded output | temporary display image | selected final artifacts | no |

## GUI layout and lifecycle

1. Wrap the existing UI in a new `main_content` frame using `grid`, with the current controls in a
   left-column frame and a new preview frame in column 1. Preserve the order and vertical layout of
   all existing controls.
2. Configure only the preview column to receive new horizontal space as needed. Do not add a new
   row below the action/status area. Keep the current minimum height.
3. Before starting preview, call `update_idletasks()` and capture
   `max(1, root.winfo_width() - 24)` as the preview pixel width. Store it with the active state so
   the later window growth cannot feed back into preview sizing.
4. The preview frame contains a centered image label and an unobtrusive placeholder while no
   preview exists. Retain the `ImageTk.PhotoImage` on the app instance so Tk does not garbage
   collect it. Create/update every Tk object only in `_drain_events`, never in the worker thread.
5. Rename the initial action to `Preview`. While it runs, disable form controls and enable the
   existing `Cancel` button as today.
6. On preview success, display the image and replace the action choices with `Render full size`
   and `Discard preview`. The full-render button uses the exact settings represented by the
   preview state.
7. `Discard preview` closes the state, clears the image reference and preview column, restores the
   `Preview` action, resets progress, and returns to validated idle state.
8. Any render-affecting variable trace or session selection change performs the same discard
   before scheduling validation. Add traces for options that currently lack them, or centralize
   value snapshot comparison before allowing full render.
9. Move the existing overwrite confirmation to `Render full size`; list panorama, optional
   coverage, and optional session-thumbnail finals exactly as today. Preview never asks to
   overwrite because it creates no final output.
10. Full rendering runs on the worker thread using the cached report. Keep the preview visible
    during rendering, update progress/status via the event queue, and let `Cancel` cancel the full
    render. On completion, keep the current history/success behavior, then close preview state.
11. On preview/full cancellation or failure, remove all attempt-owned scratch and `.partial`
    artifacts, clear the preview UI, and restore a validated `Preview` action. Never remove a
    pre-existing final.
12. `_close` must request cancellation while busy as today. Once idle, it closes preview state
    before saving settings and destroying Tk.

Use explicit worker operation names such as `validate`, `preview`, and `render`. Pass optional
render arguments by keyword while touching the call site; the current positional Boolean tail is
too easy to bind incorrectly.

## Progress behavior

- Preview phases should be clearly prefixed, for example `Preview: exposure`, `Preview: exposure
  mapping`, `Preview: compositing`, `Preview: auto contrast`, and `Preview: preparing display`.
- Full-render phase labels stay unchanged except that the exposure phase is reported as already
  complete/skipped when a cached report is used. Do not make the progress bar move backwards.
- The session-thumbnail phases remain part of the full render only.

## Focused tests

Add compositor tests in `stitcher/tests/test_compositor.py` and small GUI helper/state tests only
where logic can be tested without launching a Tk display.

1. Preview dimensions use the requested width and the existing panorama aspect/latitude rules.
2. Preview pixels match a direct render at the same dimensions for hard and feather blending,
   including local exposure normalization and incomplete-coverage behavior.
3. The preview state contains the solved report; an instrumented full render reuses it and does
   not call `_estimate_exposure_gains` again.
4. A mismatched report/frame count is rejected before output creation.
5. Full render with cached exposure is pixel-identical to the ordinary path for PNG and within the
   existing encoding tolerance for JPEG; EXR final pixels remain unchanged.
6. Full render recomputes its dimension-dependent local exposure field and auto-contrast levels.
7. Preview never creates panorama, coverage, thumbnail, history, or persistent preview files.
8. Cancelling/failing each preview phase removes scratch, memmaps, and temporary display files.
9. Discard is idempotent and leaves pre-existing final artifacts untouched.
10. Cancelling/failing the full render after preview removes new `.partial`/scratch artifacts and
    preserves pre-existing finals.
11. The captured preview width does not change after the second GUI column grows the window.
12. GUI event handling retains the `PhotoImage`, creates it on the main thread, exposes only the
    correct action for each state, and invalidates preview when options change.

## Implementation order for a lower-capability model

Implement and verify one slice at a time. Do not combine the compositor refactor and Tk layout in
one large edit.

1. Add the preview-state/fingerprint types and cleanup tests.
2. Extract cached-exposure support without changing output; run compositor tests.
3. Add the headless preview renderer and its dimension/pixel/cleanup tests.
4. Add the GUI two-column shell and width-capture helper without wiring rendering.
5. Add `preview` worker/event flow and display ownership.
6. Add `Render full size` reuse, delayed overwrite confirmation, and discard/cancel transitions.
7. Add regression tests for no persistent preview artifacts and final-output equivalence.
8. Review the diff for schema, naming, history, color, and CLI changes; none should exist except
   internal optional cached-exposure plumbing.
9. Run from `stitcher/`:

   ```text
   uv run ruff check .
   uv run ruff format --check .
   uv run mypy src
   uv run pytest
   ```

10. Manually validate on a display: normal/full-sphere and horizontal sessions, PNG/JPEG/EXR
    selections, advanced panel open/closed, window scaling, discard, cancel during preview, cancel
    during full render, overwrite refusal, and closing while busy.

Do not proceed to the next slice until its focused tests pass. If the refactor requires changing
public render behavior, output pixels, schema, CLI options, session history, or existing file
publication semantics, stop and request approval because that exceeds this feature's scope.

# Final-image Auto contrast implementation plan

## Decision and observed reference

Replace the rejected global histogram-equalization experiment with a Photoshop-style
**Auto contrast** advanced option, enabled by default. This is a final-image levels stretch,
not histogram equalization and not another per-frame exposure adjustment.

The supplied reference pair was measured after JPEG decoding:

- stitcher output: `panorama-1787615430-4-2-h1.jpg`;
- Photoshop Auto Contrast output: `panorama-1787615430-4-2-h2.jpg`;
- the fitted transform is effectively identical for R, G, and B:
  `output = clamp(1.195 * input - 23.8, 0, 255)`;
- this corresponds approximately to mapping input levels 20 and 233 to 0 and 255;
- channel correlations are above 0.998 and mean absolute fit error is below 1.9 levels.

This confirms that the useful behavior is a shared affine levels transform. It expands the
occupied tonal range while preserving channel relationships; it does not redistribute every
histogram bin. Adobe documents Auto Contrast as “Enhance Monochromatic Contrast”: shadow and
highlight values are clipped, the remaining endpoints are mapped to black and white, and all
channels receive the same correction. Adobe's documented default clips 0.5% at each end:
<https://helpx.adobe.com/photoshop/using/making-quick-tonal-adjustments.html>.

The reference JPEG may contain customized Photoshop clipping preferences, so matching its
exact inferred levels is not a stable contract. The contract is the documented monochromatic
0.5% endpoint stretch, verified against this pair for the same qualitative and color-preserving
behavior.

## User-facing contract

1. Add **Auto contrast** to the GUI Advanced options. It is checked by default and persisted
   with the existing GUI settings.
2. Add CLI paired flags `--auto-contrast` and `--no-auto-contrast`, defaulting to enabled.
3. Add `auto_contrast: bool = True` as a keyword argument to `render_session`.
4. Include `auto contrast: enabled|disabled` in the final GUI/CLI render report. Do not add
   per-strip log messages.
5. Auto contrast operates only on the completed stitched panorama. It must never influence
   source decoding, exposure estimation, exposure mapping, projection, blending, or coverage.
6. The first implementation uses fixed Photoshop-compatible shadow and highlight clipping of
   0.5% each. Do not expose clipping sliders yet.

## Output-space behavior

Photoshop's observed transform is display-referred, so JPEG and PNG must calculate and apply
Auto contrast after the existing HDR-to-SDR mapping and linear-to-sRGB conversion, but before
8-bit quantization and compression.

Use one monochromatic control histogram and one transform for every RGB channel:

1. Convert each finalized output strip to floating-point sRGB in `[0, 1]` using exactly the same
   conversion as the JPEG/PNG writers.
2. Calculate display luminance `Y' = 0.2126 R' + 0.7152 G' + 0.0722 B'` for covered, finite
   pixels. Exclude uncovered diagnostic-magenta pixels from endpoint selection.
3. Build a 4096-bin histogram of `Y'`. Locate the 0.5th and 99.5th percentile bin positions,
   with linear interpolation inside the selected bins, to obtain `black_point` and
   `white_point`.
4. If fewer than two valid samples exist, either endpoint is non-finite, or
   `white_point - black_point < 1 / 4095`, treat Auto contrast as a no-op.
5. Apply the same transform independently to all three values of each covered pixel:
   `rgb' = clamp((rgb - black_point) / (white_point - black_point), 0, 1)`.
   This shared scale and offset preserves neutral pixels and avoids the color casts caused by
   per-channel Auto Tone.
6. Quantize the transformed sRGB rows to 8-bit and pass them to the existing PNG/JPEG encoding
   path.

EXR is scene-linear and has no fixed white endpoint equivalent to level 255. The initial feature
must therefore leave EXR unchanged and report `auto contrast: skipped for EXR`; silently clipping
linear HDR values would violate the EXR contract. Keep this format decision isolated so a future
explicit HDR levels feature can be designed without changing SDR results.

## Memory-bounded render pipeline

Refactor the end of `render_session` into these ordered stages:

1. **Exposure**: existing per-frame overlap solve.
2. **Exposure mapping**: existing quarter-resolution local exposure field.
3. **Compositing**: existing projection and accumulation into disk-backed `color_scratch` and
   `weight_scratch`.
4. **Auto contrast**: finalization plus one bounded statistics pass over the final panorama.
5. **Writing**: a second bounded pass that applies the transform and streams the result.

Before the Auto contrast statistics pass, finalize the composite in place one strip at a time:

- divide feather-blended covered pixels by their weights;
- perform the existing incomplete-coverage validation;
- write diagnostic magenta for allowed uncovered pixels;
- never repeat this finalization during writing.

The Auto contrast statistics pass reads `color_scratch` and `weight_scratch` strip by strip,
converts only the current strip to floating-point sRGB, and updates the 4096-bin histogram. It
does not revisit source screenshots and does not allocate another panorama-sized image.

The writing pass reads the same finalized strips, performs the identical sRGB conversion,
applies the precomputed shared transform, and streams rows to the writer. Refactor the duplicated
PNG/JPEG conversion into a helper such as `_to_sdr_srgb(rows, encoding)`. Writers should accept
already-converted 8-bit rows, or accept the optional black/white points and call that helper;
there must be one authoritative conversion formula for the statistics and output passes.

Additional memory is bounded to one existing output strip, one temporary three-channel sRGB
strip, one luminance strip, and a 4096-entry histogram. Include these temporary strip arrays in
the existing memory estimate. Do not create a full-resolution SDR cache. JPEG may retain its
existing disk-spooled `.rgb` file, which must still be deleted on success, failure, or cancel.

Check `cancel_event` at least once per strip during finalization, histogram collection, and
writing. All temporary output files and memmaps must close before `TemporaryDirectory` cleanup,
including Windows failure paths.

## Progress behavior

Treat Auto contrast as a real fourth phase when enabled for SDR output. Each phase owns its own
completed/total counters so the progress bar resets to 0% at phase entry and reaches 100% at
phase completion. Status text should use real values:

- `[1/5] exposure 1/42`;
- `[2/5] exposure mapping 3123/5347`;
- `[3/5] compositing 1234/34534`;
- `[4/5] auto contrast 120/320`;
- `[5/5] writing 120/320`.

When Auto contrast is disabled or the output is EXR, omit that phase and use four phases with
renumbered labels. Do not show a fake zero-work Auto contrast phase.

## Tests and acceptance criteria

Add focused tests in `stitcher/tests/test_compositor.py` and GUI/CLI interface tests:

1. A synthetic low-contrast SDR composite maps its 0.5th/99.5th luminance percentiles close to
   0/1 and uses more of the output range.
2. The same black/white points are applied to R, G, and B; neutral pixels remain neutral and a
   colored ramp does not receive per-channel white balance.
3. A fixture based on the supplied before/after pair produces a transform close to the measured
   common affine mapping. Allow JPEG recompression tolerance; compare decoded pixels or inferred
   black/white points, not file bytes.
4. Auto contrast disabled produces byte-for-byte the established PNG path and pixel-equivalent
   JPEG output.
5. EXR output is unchanged whether the default option is enabled or explicitly disabled, and the
   report says it was skipped.
6. All-black, all-white, near-flat, empty-valid-mask, non-finite, and incomplete-magenta inputs
   complete without division by zero or endpoint contamination.
7. Hard and feather blending are finalized before endpoint collection; ensure feather division
   is performed exactly once.
8. Cancellation during finalization, statistics, or writing leaves no output, `.partial`, `.rgb`,
   or locked memmap files.
9. A large synthetic panorama stays within the configured memory budget; assert that no
   output-sized SDR array is allocated.
10. Progress callbacks show phase-local monotonic counters, reset at each phase, reach 100%, and
    use four or five correctly numbered phases depending on whether Auto contrast runs.

Run the complete pytest suite plus Ruff and mypy. On Windows, smoke-test JPEG and PNG through the
packaged GUI and confirm that cancelling during Auto contrast cleans all temporary files.

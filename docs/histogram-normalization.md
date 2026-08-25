# Histogram normalization implementation plan

## Contract

Add **Histogram normalization** as an advanced stitcher option. It is enabled
by default for both the GUI and CLI. The operation means a *single global,
final-output log-luminance histogram normalization*. It runs only after all
projection, overlap-local exposure compensation, blending, and coverage work
is complete. It must not alter individual captures, camera geometry, or the
seam solver. It is conventional global histogram equalization, not CLAHE or
per-channel normalization.

The operation is chroma-preserving: it changes only final linear-light
luminance by multiplying all three RGB channels by the same scalar. For HDR
EXR output, values above 1.0 remain valid and unclipped throughout. For
PNG/JPEG, apply the transform before the existing deterministic SDR conversion.
Zero, negative, non-finite, and uncovered diagnostic-magenta pixels are not
remapped. The transform is monotonic, so tonal ordering is retained. This does
not promise to preserve the original absolute exposure: users who require that
may disable the option.

## Public interface

1. Add `histogram_normalization: bool = True` to `render_session` after the
   existing keyword options. Do not make it a manifest property.
2. Add CLI paired flags:
   `--histogram-normalization` and `--no-histogram-normalization`, with the
   default set to enabled. The latter must be visible in `pano-stitch render
   --help`.
3. Add a checked **Histogram normalization** checkbox to the GUI Advanced
   options. Persist it with the other GUI preferences. Disable it along with
   other form controls while rendering.
4. Include `histogram normalization: enabled|disabled` in the final CLI and
   GUI render report. Do not add per-frame console spam.

## Final-output prepass and transform

Implement this in `stitcher/src/pano_stitch/compositor.py` as a distinct,
bounded final-output stage. It must not decode or revisit screenshots. Work
only from the existing disk-backed stitched colour accumulator.

1. Complete compositing into the current disk-backed `color_scratch` and
   `weight_scratch`. Handle coverage first: divide feather rows by their
   weights, preserve uncovered pixels for the existing incomplete-session
   behavior, and do not write the output yet.
2. Make bounded passes over output strips. For covered, finite pixels with
luminance above `1e-5`, first collect a compact coarse log-luminance histogram
to locate the robust percentile domain, then collect the 1024-bin final
histogram within that domain. Never retain more than one strip. Exclude
uncovered magenta pixels and do not use a fixed HDR maximum. If no pixels are
remappable (for example, a fully black render), skip normalization and write
the valid image unchanged.
3. Determine the global robust normalization domain from the final panorama's
   0.1th and 99.9th log-luminance percentiles. Expand a degenerate domain by
   one stop on either side.
4. Build a monotonic 1024-entry LUT that maps the input CDF to a uniform output
   CDF over that robust log-luminance domain. This is conventional global
   histogram normalization of the finished panorama. Enforce monotonicity with
   a cumulative maximum; preserve values outside the robust domain by
   extrapolating the first/last LUT slope rather than hard-clipping.
5. Make a second bounded pass over output strips. For each valid pixel with
   luminance `Y`, map `log2(Y)` with the LUT, calculate `Yt`, then multiply RGB
   by `Yt / Y`. Leave invalid, near-black, and uncovered pixels unchanged.
6. Only after this second pass, stream rows to the EXR/PNG/JPEG writer. EXR
   therefore receives normalized linear HDR values; PNG/JPEG receive the
   existing SDR transform of those values.

When the option is disabled, skip both final histogram passes and write the
current composite exactly as before, apart from progress/report text.

## Progress, cancellation, and resources

1. Add a visible `histogram normalization` progress phase covering final-image
   histogram collection, LUT construction, and final-image application.
   Progress must be monotonic across histogram, exposure estimation, exposure
   mapping, compositing, and writing.
2. Check `cancel_event` once per output strip in both histogram passes. A
   cancelled normalization must leave no output or locked scratch files.
3. Histogram state is one 1024-bin array, a 1024-entry LUT, and one existing
   output strip. Do not introduce a full-resolution source cache or a second
   output-sized memory map. Update work/progress accounting, but the configured
   memory ceiling must not increase.
4. Ensure all temporary arrays/memmaps release before `TemporaryDirectory`
   cleanup, especially on Windows.

## Tests

Add focused tests in `stitcher/tests/test_compositor.py` and interface tests
where appropriate:

1. A synthetic finished panorama produces a flatter normalized luminance CDF
   while remaining finite.
2. The RGB channel ratio of non-grey pixels is unchanged by the transform.
3. An HDR output containing values above 1.0 remains finite and is never
   clipped solely for exceeding 1.0; normalization may legitimately remap its
   luminance.
4. The LUT and output are monotonic for ascending luminance inputs, including
   values outside the robust histogram domain.
5. Disabled normalization does not invoke histogram collection/LUT application
   and matches the established exposure-local render output.
6. Hard and feather compositing results are normalized only after their normal
   blend paths complete; preserve the existing overlap and single-source-area
   tests unchanged.
7. Cancellation during final-image collection and LUT application raises
   `RenderCancelledError` and releases temporary files.
8. Serial and parallel renders remain deterministic; the resource estimator
   remains inside the configured memory budget.
9. CLI default/opt-out parsing and GUI default checkbox wiring are covered.

## Explicit non-goals

- No per-channel histogram matching; it changes hue.
- No CLAHE, per-source matching, or local contrast enhancement.
- No histogram-derived clipping of EXR highlights.
- No manifest/schema change.

The fix should replace global per-image gain with overlap-local exposure compensation, master. The current solver is
  useful; applying its gain to the entire source is the mistake.

  ## Intended algorithm

  1. Keep the existing overlap analysis and solve relative log-exposure corrections c[i].
  2. Never multiply an entire source by exp(c[i]).
  3. Build a low-cost, disk-backed correction field over the panorama:
      - For every output pixel, calculate smooth geometric weights w[i] for all covering frames.
      - Calculate C(pixel) = Σ(w[i] × c[i]) / Σw[i].

  4. While compositing frame i, apply:
      - localGain = exp(c[i] - C(pixel))
      - sample *= localGain

  5. Blend normally.

  This gives the desired properties:

  - Where only one image contributes, C = c[i], so localGain = 1. Its highlights, shadows, and original exposure remain
    untouched.

  - Inside overlaps, both images are transformed to the same smoothly varying local exposure.
  - Adding any global offset to the solved exposures changes nothing, so the stitcher cannot brighten or darken the panorama
    globally.

  - One scalar multiplier is applied to RGB in linear light, preserving colour and channel ratios.
  - EXR values remain unclipped and retain HDR range.

  ## Implementation steps

  In stitcher/src/pano_stitch/compositor.py:

  1. Preserve _estimate_exposure_gains(), but treat its results as relative exposure estimates.
  2. Remove this global operation:

  source *= exposure_report.gains[frame_position]

  3. Add a shared feather-weight helper so exposure mapping and compositing use identical edge weights.
  4. Before compositing, create one float32 disk-backed local_exposure map:
      - Accumulate weighted log gains and total weights strip-by-strip.
      - Divide them to obtain C(pixel).
      - Reuse or discard the temporary weight map afterward.

  5. Pass local_exposure and the frame’s log gain into _composite_strip().
  6. Apply exp(frame_log_gain - local_exposure) only on valid sampled pixels.
  7. Use these smooth exposure weights even when final blending is hard; this prevents exposure tearing at the selected-
     source boundary.

  8. Update progress accounting and scratch-memory estimates for the extra one-channel map.
  9. Report values as “relative exposure estimates,” not globally applied gains.

  Also finish the originally planned reliability mask: reject near-black, clipped, high-gradient, non-finite, and high-MAD
  overlap samples. That improves estimation but must not become local tone mapping.

  ## Required tests

  - Two differently exposed images: overlap has no luminance step.
  - Non-overlap pixels remain unchanged within floating-point tolerance.
  - Synthetic bright sky/dark ground: ground is not lifted because of the sky overlap.
  - HDR values above 1.0 survive EXR output without clipping.
  - Adding a constant to every solved log exposure produces identical output.
  - Hard and feather blending both remove exposure discontinuities.
  - Single-worker and parallel renders remain deterministic.
  - Updated resource estimate remains within the configured memory budget.

  Explicitly avoid histogram matching, CLAHE, global normalization, highlight compression, or per-image tone mapping. Those
  would merely replace this bug with a more sophisticated-looking bug, naturally.

# Session thumbnail implementation instructions

## Goal and fixed behavior

Add an opt-in render artifact called the **session thumbnail**. It represents a virtual camera at
the center of the capture, reconstructed from the captured source images rather than copied from
one source image or cropped from the panorama.

Use these definitions exactly:

- The virtual camera orientation is canonical session yaw `0`, pitch `0`, roll `0`. This is the
  center of both a full-sphere session and a horizontal session. Do not choose the middle frame,
  average frame poses, or use the user's current view.
- The thumbnail horizontal FoV is exactly `90.0` degrees.
- The thumbnail pixel width and height are exactly the width and height reported by
  `_source_info_for_session` for the first renderable source screenshot. The panorama Resolution
  slider and explicit-width override do not scale the thumbnail.
- Preserve the first source screenshot's aspect ratio by deriving the virtual camera's vertical
  FoV as
  `degrees(2 * atan((source_height / source_width) * tan(radians(90) / 2)))`.
  Do not reuse the session capture's vertical FoV unless it happens to equal this result.
- The thumbnail is a rectilinear perspective image, not an equirectangular crop.
- Its filename is the panorama output stem plus `-thumbnail`, with the same suffix. For example,
  `panorama-123.jpg` produces `panorama-123-thumbnail.jpg` in the same directory.
- It uses the panorama's selected output format. JPEG uses the same `jpeg_quality`; PNG and EXR
  use the same existing writers and encoding behavior.
- It uses the same `blend`, `allow_incomplete`, exposure normalization, local exposure mapping,
  SDR conversion, and `auto_contrast` option as the panorama. Auto contrast is computed for the
  thumbnail's own finalized pixels; it remains skipped for EXR.
- The option is off by default and is not persisted in GUI settings. Each new application launch
  starts with it off.
- Coverage diagnostics remain panorama-only. Do not create a thumbnail coverage file.
- CLI parity is required: add `--session-thumbnail` as a `store_true` render option, defaulting to
  false. The CLI derives the thumbnail path from `--output`; do not add a second path argument.

Do not implement the thumbnail by cropping or perspective-warping the final panorama. That loses
source detail whenever the panorama Resolution slider is below 100%, and it cannot reproduce the
same projection/blending accurately at the seam.

## Projection helper

In `stitcher/src/pano_stitch/projection.py`, add a typed helper that creates world directions for
a strip of a rectilinear virtual camera. Keep the same strip contract used by
`equirectangular_directions`: positive output dimensions, `row_offset`, and `full_height`
validation.

The helper should accept output width/height, horizontal and vertical FoV, yaw/pitch/roll, and the
strip range. For pixel centers, calculate camera-local rays with the same camera model as
`camera_maps`:

1. `focal_x = width / (2 * tan(horizontal_fov / 2))` and equivalently for `focal_y`.
2. Use principal point `((width - 1) / 2, (height - 1) / 2)`.
3. Form local rays `(pixel_x - center_x, center_y - pixel_y, focal)` (or the algebraically
   equivalent normalized-coordinate form), normalize them, then rotate them from local camera
   space into canonical world space with `_rotation_matrix(...).T`.
4. Return contiguous `float32` directions shaped `(strip_rows, width, 3)`.

Validate both FoVs as greater than zero and less than 180 degrees. Add focused projection tests:
the center ray faces canonical forward, left/right rays have the expected signs, a 90-degree
horizontal FoV reaches approximately plus/minus 45 degrees at the image edges under the existing
pixel-center convention, and strip generation equals the corresponding rows of a whole image.

## Compositor changes

In `stitcher/src/pano_stitch/compositor.py`, extend `render_session` with a final keyword argument
`session_thumbnail: bool = False`. Keep existing callers source-compatible. Derive the thumbnail
path internally with a small helper so GUI, CLI, overwrite checks, and compositor tests use one
authoritative naming rule.

Do not duplicate the panorama renderer wholesale. Extract only the narrow reusable operations
needed for a second projection target:

- compositing arbitrary supplied world-direction strips;
- mapping the solved per-frame log gains into a local exposure field for arbitrary directions;
- normalizing feather weights and enforcing incomplete-coverage behavior;
- calculating optional auto-contrast levels; and
- selecting/writing EXR, PNG, or JPEG through the existing writer classes.

Keep `equirectangular_directions` as the panorama direction producer and use the new rectilinear
direction helper as the thumbnail producer. A practical narrow implementation is to leave the
panorama pass structurally intact, then run a second bounded thumbnail pass inside the same
`render_session` temporary directory. Reuse the already solved `ExposureReport`, but it is fine to
decode each source again for the thumbnail; do not retain every decoded source in memory.

For local exposure correction on the thumbnail, calculate per-thumbnail-pixel weighted log gain
using the same `camera_maps` validity/edge-distance results and `_exposure_weight` values used for
compositing. Divide accumulated weighted log gains by accumulated weights, exactly as the panorama
exposure map does. Pass that field to the thumbnail compositing operation so
`_local_exposure_multiplier(log_gain, local_exposure)` is unchanged. Do not sample the panorama's
quarter-resolution exposure field: that would couple thumbnail quality to panorama resolution.

Use a render plan based on the thumbnail width and height and the existing memory budget. Its
scratch arrays must be disk-backed memmaps, as the panorama arrays are. Include thumbnail scratch
bytes in `estimate_render_resources` only when that API is explicitly extended to accept the
feature flag; otherwise report the maximum concurrently allocated scratch, not the sum, because
the two passes are sequential. Do not allocate a full extra panorama or hold both output targets'
scratch arrays concurrently.

Generate temporary output files in the destination directory. Publish the panorama, optional
coverage file, and thumbnail only after every requested artifact has been written successfully.
On cancellation or any exception before publication, delete all temporary artifacts and leave any
pre-existing final files untouched. This requires delaying the current `os.replace` calls until
both output passes finish. Before publishing, ensure all writers are closed. Multiple final
`os.replace` operations are not one filesystem transaction: either add a small publish helper that
moves existing finals to temporary backups and restores them if any replacement fails, or clearly
limit rollback tests to failures before publication. Do not claim that several plain sequential
renames are atomic.

Extend progress reporting with explicit thumbnail phases when enabled. Preserve the current phase
labels when disabled. When enabled, count and label thumbnail exposure mapping, compositing,
optional auto contrast, and writing; cancellation must be checked in every thumbnail strip/frame
loop.

## GUI changes

In `stitcher/src/pano_stitch/gui.py`:

1. Add `self.session_thumbnail_var = tk.BooleanVar(value=False)` in `_build_variables`.
2. Add a normal `ttk.Checkbutton` labeled `Generate session thumbnail` directly below the
   Resolution row in the main `Render options` frame. It must appear before the Advanced options
   button, not inside `advanced_frame`. Shift the Advanced options button down by one grid row.
3. Do not load or save this variable in `gui-settings.json`.
4. Add the derived thumbnail path to the pre-render overwrite check when the checkbox is enabled.
   The confirmation must list panorama, optional coverage, and thumbnail files that already exist.
5. Pass the Boolean value to `render_session` by keyword. While touching this call, convert the
   optional tail arguments to keywords so adding another Boolean cannot silently bind to the wrong
   parameter.
6. Include the thumbnail path in the success status when enabled, but continue storing only the
   panorama filename in stitched-session history.
7. Ensure `_form_control_widgets` includes the new checkbutton so it is disabled during validation
   and rendering like the other form controls.

The output preview/name field continues to describe the panorama only. The thumbnail name is
derived and is not separately editable.

## CLI changes

In `stitcher/src/pano_stitch/cli.py`, add `--session-thumbnail` to the `render` subcommand and pass
it to `render_session` by keyword. If enabled, print a second final `wrote ...` line for the derived
thumbnail path. Do not let `--resolution` or `--width` change thumbnail dimensions.

## Tests and acceptance criteria

Add tests primarily to `stitcher/tests/test_compositor.py`; add GUI/CLI unit tests only if the
current project has a suitable pattern rather than introducing a large UI harness.

Required compositor tests:

1. With the flag omitted/false, output behavior is unchanged and no `-thumbnail` file exists.
2. With a synthetic directional scene and the flag true, the thumbnail exists, has exactly the
   first source dimensions, and its center/corners correspond to a yaw-0, pitch-0 rectilinear
   90-degree view within interpolation/encoding tolerance.
3. Rendering a low-resolution or explicitly sized panorama does not change thumbnail dimensions
   or pixels.
4. A non-square source produces the source aspect ratio and the derived vertical FoV, without
   stretching.
5. PNG, JPEG, and EXR thumbnails use the panorama suffix. JPEG exercises the selected quality
   path; EXR preserves the existing float encoding/compression contract and skips auto contrast.
6. Feather and hard blend paths both produce covered output. Exposure-normalized sources verify
   that thumbnail exposure uses the solved gains/local field rather than raw source brightness.
7. `allow_incomplete=False` rejects uncovered thumbnail pixels; `allow_incomplete=True` uses the
   existing magenta uncovered-pixel behavior.
8. Cancellation or an injected thumbnail write failure before publication leaves no newly
   published panorama, thumbnail, coverage, `.partial`, or scratch files and does not overwrite
   existing finals. If publication rollback is implemented, inject a rename failure too and prove
   that all previous final files are restored.
9. The thumbnail filename helper handles `.jpeg` as well as `.jpg`, `.png`, and `.exr`.

Run from `stitcher/` and fix every warning/error:

```text
uv run ruff check .
uv run ruff format --check .
uv run mypy src
uv run pytest
```

Keep the implementation narrow. Do not change the capture manifest/schema, session history shape,
source validation rules, panorama dimensions, coverage semantics, default render settings, or
existing output color pipeline.

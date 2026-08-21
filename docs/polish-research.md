# Post-MVP stitching polish research

## Exposure normalization

Cyberpunk's automatic exposure can change between panorama poses. Geometrically
correct feathering cannot hide a brightness discontinuity if neighboring source
images have different exposure.

The recommended first implementation is a global multiplicative gain per
source image, calculated before compositing in linear light:

1. Use the recorded poses and FoV to sample only known overlap regions at a
   reduced resolution.
2. Decode PQ input to linear light, calculate robust luminance ratios, and
   discard near-black, clipped, HUD, and high-gradient samples.
3. Build a connected overlap graph. Each edge estimates the log-luminance
   difference between two frames.
4. Solve weighted least squares for one log gain per frame, anchoring a
   representative central-exposure frame. Clamp the final correction to a
   conservative configurable range (default: plus or minus one EV).
5. Apply the gain to linear RGB before feathering and before SDR tone mapping
   or EXR output.

This mirrors established photometric panorama workflows: Hugin estimates
exposure from pixel groups in matching overlap regions and permits an exposure
anchor. OpenCV also exposes gain, channel, and block-based exposure
compensators. The first PanoramaCapture version should use only global
luminance gain: per-channel white balance and spatial/block gain can create
color shifts or gradients when a game's temporal exposure changes locally.

Packet 12 decision: exposure normalization is mandatory and automatic for
production renders, rather than an opt-in GUI setting. The render report must
state the anchor image, overlap graph connectivity, and each applied gain. A
disconnected graph is an error, not an invitation to guess.

## Optional upright / horizon correction

The capture poses provide a consistent game-space up direction, but that does
not guarantee the chosen view feels level. A purely automatic correction is
unsafe in Cyberpunk: slanted architecture, cables, signs, ramps, and intentional
Dutch angles can outvote the real horizon.

The recommended workflow is therefore preview-first and optional:

1. Produce a low-resolution equirectangular preview from the metadata-driven
   stitch.
2. Reproject several equatorial rectilinear preview views, rather than detecting
   lines directly in equirectangular space where straight world lines curve.
3. Run OpenCV's Line Segment Detector or probabilistic Hough detector on those
   views, lift segments back to spherical directions, and compute a robust
   candidate world-up rotation from consensus horizontal/vertical evidence.
4. Present the candidate as a suggestion. The GUI exposes enable/disable,
   correction-strength (0 to 100 percent), and manual pitch/roll fine tuning.
5. Apply the selected correction as one 3D rotation of output directions during
   final projection. Do not alter per-frame metadata, camera calibration, or
   source-image alignment.

The user must be able to leave correction at zero. The GUI should visibly mark
weak/ambiguous line consensus and default to no change in that case.

## Sources

- OpenCV exposes `ExposureCompensator` implementations including global gain,
  channel, and block gain variants:
  <https://docs.opencv.org/4.12.0/d2/d37/classcv_1_1detail_1_1ExposureCompensator.html>.
- Hugin's photometric optimizer calculates exposure from matching overlap
  samples and supports an exposure anchor:
  <https://hugin.sourceforge.io/docs/manual/Vig_optimize.html>.
- OpenCV documents `HoughLinesP` and line-segment features:
  <https://docs.opencv.org/4.12.0/dd/d1a/group__imgproc__feature.html>.
- Upright adjustment for spherical panoramas is a documented specialized
  problem, supporting the preview-first approach rather than a naive flat-image
  line detector:
  <https://cg.postech.ac.kr/papers/sphericalPano.pdf>.

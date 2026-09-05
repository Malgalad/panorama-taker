# PNG optimizer benchmark — 2026-09-05

Historical experiment: the optional direct OxiPNG path was subsequently
[integrated with WIC fallback](native-oxipng-integration.md). The other experimental
encoder helpers/dependencies were removed; reproduction paths below describe
the original experiment, not the current source tree.

## Decision

Do not add a post-encoding optimizer to the production pipeline based on these
results. The tested two-stage lossless route is slower than the parked custom
encoder and uses much more memory. pngloss is too slow. pngquant can make much
smaller files, but introduces visible dithering and is not a transparent PNG
replacement.

The promising discovery is **direct OxiPNG/libdeflate encoding**, not PNG
optimization after encoding. Its level-3 configuration takes 35.0–35.2% less
encoding time than contemporaneous WIC controls, with only 1.65–1.67% larger
files and exact pixels. It also beats the parked libpng/zlib-ng candidate by
9.7–10.1% in time and approximately 1.8% in output size. However, this raw-image
prototype uses 1037–1042 MiB of physical RAM, exceeding the project's 953 MiB
working-set ceiling even before integrating it with rendering.

Next step, if approved: investigate a memory-bounded direct libdeflate design
and its compression tradeoff. OxiPNG RawImage is whole-image, and
[libdeflate has no streaming API](https://github.com/ebiggers/libdeflate#api).
Do not assume this benchmark can be dropped into the existing strip writer.
The lower-risk streaming libpng/zlib-ng candidate remains parked; see
[the preceding experiment](native-png-encoder-benchmark.md).

No production writer, GUI defaults, HDR pipeline, or D3D12 code changed.
Only opt-in benchmark helpers and documentation were extended.

## Inputs and measurement boundaries

Windows x64, Ryzen 7 9800X3D, MSVC RelWithDebInfo. Two existing RGB8 panoramas,
17552 × 8776 (154 million pixels), hard and feather blends of the same scene:

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| `lifetime-png-hard-1.png` | 160,567,130 | `de4bb9d1487b19235ee80eeb32e7d4b18080935f835855cf80420f2ae8d0962e` |
| `lifetime-png-feather-1.png` | 154,557,185 | `a96159d3ee6ec3ffb7e9711cdc586cc8b14f77517b1382ee37a7429c4e9abd12` |

Originals remain under `C:\dev\pano-stitch-native-stage\render-bench-batch2`.
Results/logs are under `C:\dev\pano-stitch-native-stage\png-opt-bench`.
These are two blends, not a diverse image corpus. Existing stitch boundaries
are part of the reference, not optimizer artifacts.

- Direct encoder times exclude initial input decoding/raw-file loading and
  subsequent correctness checks; they include encoding, writes, and close.
- CLI optimizer times include process startup, file reading, decoding, processing,
  and output writing. They are **additional** to the initial PNG encoder.
- Runs were serial, without competing benchmarks, builds, or image analysis.
  These are warm/local-file measurements, not cold-disk or durable-flush tests.
- WIC and libpng RAM is sampled during encoding, including the resident 441 MiB
  RGB input. OxiPNG/CLI RAM is process peak working set, sampled by the wrapper
  every 10 ms. Private commit is also retained in JSON, but is not physical RAM.
  These standalone figures are not integrated renderer resource measurements.
- CPU-only codecs: no GPU work submitted; VRAM counters were not measured.
  No AMD/Intel GPU hardware verification is claimed.
- Current WIC controls are faster than the earlier experiment; all percentages
  here use the current controls rather than comparing different sessions.

## 1. Lossless: OxiPNG

OxiPNG 10.2.1. Direct helper: serial Rust build, default features disabled,
libdeflater/libdeflate 1.26.0, one MinSum filter, no color/depth/palette reduction,
explicit `sRGB` intent 0 and `gAMA=45455`. Cargo.lock pins resolved dependencies.

Three-run medians for WIC, parked libpng, and direct levels 1/3; level 5 has one
screening run per input. Encoder order was reversed between repeat rounds.

| Encoder | Hard seconds | Feather seconds | Size vs WIC, hard / feather | Peak RAM MiB, hard / feather |
| --- | ---: | ---: | ---: | ---: |
| WIC production control | 7.668 | 7.506 | baseline | 502.7 / 502.7 |
| Parked libpng level 3, memory 9 | 5.523 | 5.402 | +3.57% / +3.57% | 450.6 / 450.6 |
| Direct OxiPNG, libdeflate 1 | 3.463 | 3.399 | +6.15% / +6.22% | 1049.6 / 1042.9 |
| Direct OxiPNG, libdeflate 3 | 4.967 | 4.880 | +1.67% / +1.65% | 1042.5 / 1036.6 |
| Direct OxiPNG, libdeflate 5 | 6.087 | 5.985 | −0.25% / −0.27% | 1039.6 / 1033.8 |

Direct level 1 saves about 55% of encoding time but grows files by about 6.2%.
Level 3 is the more conservative compression-first tradeoff. Level 5 is useful
if matching/slightly improving WIC size matters more than maximum speed.

### Actual second-pass measurements

CLI single-filter settings: `-o 5 -f 5 --zc LEVEL --nx -t 1 --force --out NEW INPUT`.
`--force` forces creation of a **new named output**, never overwrites an original.
No metadata stripping. Each entry below is one screening run per blend.

| Input and optimizer | Hard pass seconds | Feather pass seconds | Peak RAM MiB, approximate |
| --- | ---: | ---: | ---: |
| Original WIC PNG → OxiPNG preset 0 (`-o 0 --nx -t 1`) | 11.149 | 10.821 | 1557–1584 |
| Original WIC PNG → single filter, libdeflate 1 | 4.280 | 4.185 | 1338–1355 |
| Original WIC PNG → single filter, libdeflate 3 | 5.987 | 5.821 | 1332–1349 |
| Original WIC PNG → single filter, libdeflate 5 | 6.985 | 6.828 | 1329–1346 |
| Uncompressed PNG → single filter, libdeflate 1 | 4.552 | 4.454 | 1926–1931 |
| Uncompressed PNG → single filter, libdeflate 3 | 6.198 | 6.113 | 1919–1925 |

The uncompressed libpng first pass itself takes 2.628 / 2.549 seconds and writes
462,843,491 bytes. Thus the **measured two-stage sum** is 7.180 / 7.003 seconds at
level 1, or 8.826 / 8.661 seconds at level 3. Neither beats the parked encoder;
level 1 also has larger output. A different fast first encoder was not tested
here, so this is not proof that every conceivable two-stage implementation loses.

All six unique direct OxiPNG outputs were independently decoded with Pillow and
compared over every RGB pixel: zero error. SHA-256 checks show the corresponding
CLI, preset-0, repeat, and two-stage outputs are byte-identical to these verified
outputs. All completed root-level test PNGs passed chunk CRC/framing checks;
OxiPNG retained the expected RGB8 dimensions and explicit color-tag payloads.
The C++ controls additionally passed independent production-WIC decode and
pixel/tag verification. This is focused output verification, not a universal
PNG conformance certification.

## 2. Full-color lossy: pngloss

Pinned upstream commit `559f09437e1c797a1eaf08dfdcddd9b082f0e09c`.
The benchmark wraps its unchanged RGB optimization algorithm, default bleed 2,
then encodes its selected row filters through tagged libpng/zlib-ng level 3,
memory 9. Timing includes the RGB working copy, optimization, and encoding.
A Windows `gettimeofday` shim supports upstream progress code; no algorithm fork.

Full-size hard, strength 3: **stopped after 60.052 seconds**, no completed PNG,
peak working set 892.0 MiB. This is a timeout lower bound, not a completed encode
time. No full-size quality conclusion is available. Strength 6/full-size feather
were not pursued after the speed rejection.

A 1600 × 800 overview was tested separately; do not extrapolate its perceptual
scores to native resolution:

| Strength | Encode seconds | Bytes | Max RGB error | RGB RMSE | SSIMULACRA2 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0 | 1.043 | 2,302,709 | 0 | 0 | not needed: exact pixels |
| 3 | 1.059 | 1,364,531 | 4 | 1.505 | 86.55 |
| 6 | 1.079 | 1,225,395 | 9 | 2.620 | 75.90 |

Color tags and production-WIC decoding passed all three small outputs. Lossy
strengths are explicitly permitted to differ in the experimental probe only.

## 3. Palette lossy: pngquant/libimagequant

pngquant 3.0.3 built with its published Cargo.lock (`cargo install pngquant
--version 3.0.3 --locked`), imagequant 4.2.2. This deliberately reproduces that
release's dependency set, not the latest possible imagequant. Cargo warned that
the upstream lock includes yanked `rgb 0.8.37`; this is not a production dependency
recommendation. Default dithering retained; quality controls use pngquant's own
scale, not SSIMULACRA2.

| Setting | Hard pass seconds | Feather pass seconds | Output bytes, hard / feather |
| --- | ---: | ---: | ---: |
| speed 8, quality 90–100, 1 worker | 24.457 | not run | 65,295,123 / — |
| speed 10, quality 90–100, 1 worker | 12.518 | rejected at 2.588 | 66,292,275 / no output |
| speed 10, quality 0–100, 1 worker | 12.490 | not run | 66,292,275 / — |
| speed 10, quality 0–100, 8 workers | 5.491 | 5.355 | 66,292,275 / 64,657,460 |

Successful full-size peak working set: about 741 MiB; private commit about
744 MiB. Worker count controlled with `RAYON_NUM_THREADS`. Feather conservative
run returned the documented quality-rejection exit 99, not a crash. Hard outputs
at speed 10 were byte-identical across quality thresholds and worker counts.

The eight-worker outputs are 58.7% / 58.2% smaller than WIC. Adding the measured
uncompressed first-pass time gives an **estimated**, not directly measured,
two-stage total of 8.119 / 7.903 seconds. pngquant here read the original compressed
PNG, not the uncompressed intermediate; its decode/I/O cost could differ there.
A direct libimagequant RGB integration was not benchmarked. No such estimate is
needed to reject transparent replacement, because visible loss was demonstrated.

### Perceptual verification

Independent full-image RGB comparison, speed 10/eight workers:

| Blend | RGB MAE | RGB RMSE | Maximum channel error | Changed pixels | Crop SSIMULACRA2 min / median / max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Hard | 1.978 | 3.221 | 169 | 98.24% | 55.21 / 78.44 / 86.73 |
| Feather | 1.998 | 3.295 | 139 | 98.62% | 52.60 / 77.76 / 86.79 |

Official libjxl 0.12.0 `ssimulacra2.exe`: 15 native 1024 × 1024 crops per image,
centers at x = 5/25/50/75/95%, y = 20/50/80%. No resize before metrics. These
sample about 10.2% of each image; they are not whole-panorama perceptual proof.
Reference and candidate crops carry identical explicit sRGB/gAMA metadata.

Visual inspection of the worst matching native crop (09) in both blends shows
added stippling/dither on the smooth blue panels, green bottles, and yellow
signage/highlights. Not visually lossless. Full PNG color metadata remained
correct (`gAMA=45455`, `sRGB=0`); the changed color type is indexed RGB, as expected.
The slower speed-8 candidate was not separately perceptually qualified.

## Reproduction and verification

Helpers live in `stitcher/native/tests/png-encoder-benchmark`:

- Enable the existing default-OFF `PANO_BUILD_PNG_ENCODER_BENCHMARK` only for the
  Windows experiment. `probe INPUT OUTPUT pngloss STRENGTH` tests pngloss;
  `probe INPUT OUTPUT png 0` produces the uncompressed intermediate.
- Build `oxi-raw/Cargo.toml` with `cargo build --release --locked`. Its CLI is
  `pano-oxi-raw INPUT.rgb NEW.png WIDTH HEIGHT LEVEL`.
- `images.py prepare INPUT.png NEW_DIRECTORY` produces RGB raw data/overview.
  `images.py compare INPUT.png NEW_DIRECTORY --candidate OUTPUT.png` computes
  full RGB differences and matched crops; add `--no-crops` for lossless checks.
  Requires Pillow and NumPy (tested 12.3.0 / 2.2.6 in a private environment).
- `measure-tool.ps1` records time, output bytes, working/private memory, exit,
  and timeout status in sidecars; refuses existing output/log paths. The 60 s
  timeout killed only its own pngloss child and correctly reported failure.
- Cargo.lock retained for direct OxiPNG; Rust 1.98.1 used in a private Windows
  toolchain. The Windows benchmark option was restored to OFF after testing.

Checks actually run:

- Windows experimental MSVC build passed; wrapper uses `/W4 /WX`. Upstream
  pngloss emitted four unknown `#pragma unused` warnings (not wrapper warnings).
- Normal Linux Release configure/build and CTest: 4/4 passed.
- Normal Windows RelWithDebInfo configure/build and CTest: 8/8 passed, including
  GUI self-test and WARP preflight. Release-directory GUI was not rebuilt here.
- Focused Python checker test passed (equality, known error, overwrite refusal,
  dimension mismatch); Black, Ruff, C/C++ clang-format checks passed.
- Rust Clippy with `-D warnings` passed; Rust helper formatted with rustfmt.
- PowerShell helper parsing and existing-output refusal passed. Black checking
  required running outside the restricted process sandbox after workers stalled.
- No new TypeScript or frontend changes. No game/capture operations.

This is experimental RGB8 code, not production-ready cancellation, streaming,
staging/failure cleanup, grayscale, or HDR support. Benchmark outputs and raw
intermediates are disposable. Cleanup removed 44 generated PNGs (including the
empty timed-out output) and two raw RGB files: 8,190,060,433 bytes. Logs,
comparison JSON, diagnostic crops, three small pngloss outputs, and
`pngquant-10-parallel8-{hard,feather}.png` remain for inspection. Both original
panorama hashes were rechecked unchanged. Removed files can be regenerated.

## Upstream references / licensing

- [OxiPNG manual](https://github.com/oxipng/oxipng/blob/v10.2.1/MANUAL.txt),
  [raw API](https://docs.rs/oxipng/10.2.1/oxipng/struct.RawImage.html): MIT.
- [pngloss source](https://github.com/foobaz/pngloss): GPL-3.0; benchmark-only.
- [pngquant](https://pngquant.org/) and [libimagequant](https://pngquant.org/lib/):
  GPL/commercial licensing requires review before distribution/integration.
- [SSIMULACRA2](https://github.com/cloudinary/ssimulacra2) and
  [libjxl 0.12.0 binaries](https://github.com/libjxl/libjxl/releases/tag/v0.12.0).
- [PNG chunk ordering](https://www.w3.org/TR/png-3/#5ChunkOrdering).

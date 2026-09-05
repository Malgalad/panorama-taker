# Custom PNG encoder benchmark

Historical experiment: these experimental encoder targets/dependencies were
subsequently removed. Only [optional OxiPNG](native-oxipng-integration.md) was
integrated; the libpng/zlib-ng candidate below was not shipped.

## Recommendation

The best compression-first candidate among the tested settings is **libpng +
zlib-ng, compression level 3, memory level 9**, with adaptive PNG filtering and
the normal filtered DEFLATE strategy. Corrected three-run medians show 26.2–27.5%
less encoding time for 3.57% more bytes than WIC on these inputs. It reduces
sampled encoding-phase working set by approximately 52 MiB.

The larger table saves only about 0.1% versus ordinary libpng level 3; timing is
essentially tied (about 0.3% slower in the final medians). This is a slightly
better compression tradeoff, not a separate meaningful speed improvement.
Other tested levels did not offer a more attractive compression-first compromise.
No production integration or GUI default change has been made.

## Scope

The previous WIC filter experiment was reverted, retaining only
`native-png-filter-benchmark.md`. Production encoding still uses unchanged WIC.
The new comparison is an opt-in test executable, not a GUI or CLI codec change.

Measured on 2026-09-05 with Windows x64 MSVC RelWithDebInfo on an AMD Ryzen 7
9800X3D. The inputs are the
existing 17552 x 8776 RGB8 hard/feather panorama outputs from session
`pano-1787897185-2`: two blends of one scene, not a diverse image corpus.
After the initial results, the user explicitly accepted approximately 25% less
encoding time for 3.67% more file size, while requesting a better tradeoff if possible.

## Candidates

- [libpng 1.6.58](https://github.com/pnggroup/libpng/tree/v1.6.58), using adaptive
  PNG filtering and its normal filtered DEFLATE strategy (`png`).
- The same libpng with `Z_DEFAULT_STRATEGY` (`png-default`). This changes the
  compressor strategy, not the adaptive PNG filter selection.
- The same libpng with normal strategy and compressor memory level 9 instead of
  8 (`png-mem9`), increasing the small, bounded compressor workspace.
- [libspng 0.7.4](https://github.com/randy408/libspng/tree/v0.7.4), using its
  default adaptive filtering and filtered DEFLATE strategy (`spng`).

Both libraries use [zlib-ng 2.3.3](https://github.com/zlib-ng/zlib-ng/tree/2.3.3)
in zlib-compatible mode. The benchmark CMake file pins archive SHA-256 hashes.
Libraries are static, native-machine-only compilation is disabled, and CPU
instruction selection uses zlib-ng's runtime detection. No D3D12 code changes or
GPU encoding are involved; AMD/Intel GPU execution is not verified here.

## Method

`pano_png_encoder_probe` decodes the input once before timing. The timed interval
includes encoder creation, file writes, finalization and close. WIC uses the
unmodified production writer with 1024-row bands, including its RGB-to-BGR packing.
Custom encoders consume RGB rows directly and use progressive/scanline APIs.
All runs are serial, without concurrent builds or tests.

After timing, the probe independently decodes the output through production WIC
and compares every RGB byte with the source. It also checks dimensions, channels,
sample type, primaries, transfer function and cICP. The experimental wrappers
now explicitly write and verify the production writer's `sRGB` perceptual intent
and `gAMA=45455` tags. The original field-only check missed their omission because
the inspector defaults untagged RGB8 to sRGB; full chunk inspection caught this.
The corrected finalists were subsequently remeasured under label `tagged`.
The wrappers
only support RGB8; they are not production replacements for grayscale, HDR,
metadata preservation, cancellation or staged-file cleanup.

Working set and private commit are sampled every 5 ms during encoding. They include
the benchmark's resident 441 MiB RGB input, but exclude the later verification
buffer. Production rendering remains banded; these are not end-to-end renderer
memory figures. No D3D12 work is submitted; VRAM counters are not measured.

## Final confirmation: metadata-correct outputs

Three samples per encoder and blend, with normal/reverse/normal encoder order.
Times below are medians in seconds, with observed minimum–maximum in parentheses.

| Encoder setting | Hard time | Feather time | Hard bytes | Feather bytes |
| --- | ---: | ---: | ---: | ---: |
| WIC, unchanged | 8.630 (8.614–8.655) | 8.622 (8.495–8.729) | 160,567,130 | 154,557,185 |
| libpng level 3, memory 8 | 6.352 (6.323–6.422) | 6.237 (6.223–6.341) | 166,457,167 | 160,231,414 |
| libpng level 3, memory 9 | 6.370 (6.315–6.426) | 6.254 (6.134–6.319) | 166,291,847 | 160,076,125 |

Sampled peak working set was approximately 502.7 MiB for WIC and 450.6 MiB for
libpng memory level 9; private commit was approximately 496.2 and 444.7 MiB.
The main memory saving comes from consuming RGB rows without the production WIC
writer's separate packed BGR band. The benchmark's resident source is included
in both numbers. No additional full-image buffer is required by the custom encoder.

All 18 corrected full-resolution outputs passed pixel equality and explicit SDR
tag checks. Earlier sweeps and preliminary confirmation were faster in absolute
terms; use paired controls within a sweep, not the fastest samples from different
sweeps. Host conditions were not otherwise controlled. This is an encoding-only
gain, not a claim that the entire final-render pipeline is 26–27% faster.

## Verification and remaining work

- MSVC benchmark wrapper build passed `/W4 /WX` after the metadata correction.
- Normal Windows build and CTest passed 8/8, including GUI and WARP tests; the
  staging configuration was restored to benchmark OFF.
- Linux Release configure/build and CTest passed 4/4.
- Ten initial RGB8 fixture round trips covered all variants at levels 3/4 and
  Unicode/space-containing output paths. Five more round trips checked explicit
  SDR tags after the correction.
- Five negative checks passed: existing-output refusal with unchanged hash,
  invalid encoder, invalid levels 0/10, and rejection of RGB16 HDR input without
  creating output. PowerShell syntax/order checks and clang-format/diff whitespace
  checks passed.
- Independent raw-chunk validation passed CRCs, bounds, first/last chunk rules,
  consecutive IDATs, and color-tag placement on all 91 generated PNGs. All 18
  corrected full-size outputs had byte-identical non-IDAT chunks to their source;
  all five corrected fixtures had matching metadata (libspng orders the two color
  tags differently). All 16 full-size WIC controls matched source file SHA-256.
  The two tested DEFLATE strategies produced byte-identical files in eight pairs.

Chunk ordering and SDR signaling were checked against the
[W3C PNG specification](https://www.w3.org/TR/png-3/#5ChunkOrdering).
Both SDR color tags must precede image data; their relative order is not mandated.

Before production adoption, integrate through the existing banded writer boundary
and cover grayscale output, cancellation, staged-file failure cleanup, all writer
metadata contracts, and dependency licensing/packaging. Benchmark more scenes.
Neither these checks nor the existing suite establish universal regression freedom;
actual AMD/Intel GPU testing remains deferred and no GPU code changed here.

## Initial sweep

One sample per setting and blend; times are seconds. The WIC baseline sizes are
160,567,130 bytes (hard) and 154,557,185 bytes (feather).
These exploratory custom outputs omitted the two SDR tags (29 bytes); pixels
round-tripped, but these files did **not** pass the later full metadata check.

| Encoder | Level | Hard time | Hard bytes | Feather time | Feather bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| WIC | unchanged | 7.670 | 160,567,130 | 7.506 | 154,557,185 |
| libspng | 1 | 5.857 | 263,856,403 | 5.729 | 255,409,629 |
| libspng | 3 | 7.831 | 166,457,138 | 7.688 | 160,231,385 |
| libspng | 6 | 15.083 | 158,698,280 | 14.799 | 152,322,620 |
| libspng | 9 | 81.859 | 153,757,316 | 86.601 | 147,187,692 |
| libpng | 1 | 3.689 | 263,856,403 | 3.606 | 255,409,629 |
| libpng | 3 | 5.719 | 166,457,138 | 5.596 | 160,231,385 |
| libpng | 6 | 12.891 | 158,698,280 | 12.796 | 152,322,620 |
| libpng | 9 | 79.564 | 153,757,316 | 84.827 | 147,187,692 |

The libraries produce equal sizes at each level, but libpng is consistently faster
here. Level 1's roughly 64–65% larger output is not an attractive tradeoff.

## Intermediate levels and compressor knobs

One sample per setting and blend, seconds. The second sweep's WIC controls were
7.760 seconds (hard) and 7.475 seconds (feather).
These custom outputs also predate the metadata correction.

| Variant | Level | Hard time | Hard bytes | Feather time | Feather bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| png | 2 | 4.555 | 175,900,420 | 4.361 | 169,705,623 |
| png | 3 | 6.253 | 166,457,138 | 5.810 | 160,231,385 |
| png | 4 | 7.453 | 162,457,432 | 7.180 | 156,232,938 |
| png | 5 | 8.343 | 161,388,206 | 8.104 | 155,117,665 |
| png-default | 2 | 4.576 | 175,900,420 | 4.513 | 169,705,623 |
| png-default | 3 | 5.965 | 166,457,138 | 6.200 | 160,231,385 |
| png-default | 4 | 7.425 | 162,457,432 | 7.350 | 156,232,938 |
| png-default | 5 | 8.105 | 161,388,206 | 8.190 | 155,117,665 |
| png-mem9 | 3 | 5.715 | 166,291,818 | 5.535 | 160,076,096 |
| png-mem9 | 4 | 7.351 | 162,316,885 | 7.241 | 156,103,351 |

The alternate DEFLATE strategy gives identical sizes at levels 2–5. Level 2 costs
about 9.6–9.8% more bytes; level 4 sacrifices most of the speed gain for a roughly
1% size increase; level 5 is slower than WIC. Memory level 9 saves about 0.1% of
the level-3 file size. These are discrete settings, not proof of a global optimum.

## Reproduction

On Windows x64 with MSVC and CMake 3.24 or newer, configure the native tree with
`-DBUILD_TESTING=ON -DPANO_BUILD_PNG_ENCODER_BENCHMARK=ON` and build target
`pano_png_encoder_probe` in RelWithDebInfo. The option defaults OFF; normal product
builds neither fetch nor link these experimental dependencies.

Run `stitcher/native/tests/png-encoder-benchmark/run.ps1` with `-InputDirectory`,
`-ArtifactDirectory`, and a fresh `-Label`. Inputs must be named
`lifetime-png-hard-1.png` and `lifetime-png-feather-1.png`. Optional `-Executable`,
`-Encoders`, `-Levels`, and `-Iterations` control the comparison. Existing output
and log files are never overwritten. Encoder order reverses on even iterations.
The probe can also encode individual RGB8
fixtures with arguments `INPUT OUTPUT ENCODER LEVEL`.

Local raw evidence: `C:\dev\pano-stitch-native-stage\png-encoder-bench`.
The 91 generated image copies (12,614,945,658 bytes) were removed after verification;
the input panoramas and per-run logs are retained. Outputs can be regenerated.
Labels: `sweep` (levels 1/3/6/9), `middle` (levels 2/3/4/5 and strategies),
`memory` (memory level 9), `confirm` (three preliminary runs of the finalists at
level 3), and `tagged` (three corrected, metadata-complete runs).
The preliminary confirmation used normal order once, then reverse order twice
because PowerShell array aliasing mutated the original list. The script now
clones the array; corrected confirmation alternates normal/reverse/normal.

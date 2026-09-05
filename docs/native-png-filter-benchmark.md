# WIC PNG filter benchmark

## Decision

Keep the existing unspecified/default WIC filter. None of the explicit filters
met the requested compression-first tradeoff (approximately 10% less encoding time
for at most 1% more file size). No GUI/CLI default or compression level changed.
The experimental filter option, probe, and tests were subsequently reverted at the
user's request. Only this results document was retained; the reproduction notes
below describe the removed experiment, not currently available build targets.

## Method

Measured on 2026-09-05 using the Windows x64 MSVC RelWithDebInfo build and the
existing 17552 x 8776 RGB8 hard/feather panorama outputs from session
`pano-1787897185-2`. This is two blends of one scene, not a diverse image corpus.

`pano_png_filter_probe` decodes the input once outside the timer, then uses the
production image writer with 1024-row bands. Time includes writer creation,
RGB-to-BGR packing, WIC encoding, file writes and finalization; decoding and
post-encode verification are excluded. No GPU is used by this codec-only probe.
The source image is held in host RAM for the benchmark; this does not change the
production render pipeline's banded memory handling.

Each result is decoded and compared pixel-for-pixel. The inspected encoding fields
are compared too. A separate binary chunk comparison confirmed all non-IDAT chunks
were identical to the source in all 14 initial outputs. Default and Adaptive also
produced identical complete file SHA-256 hashes to the original panorama outputs.

## Initial sweep

Times are seconds. Size deltas are relative to the default for each blend.

| Filter | Hard time | Hard size delta | Feather time | Feather size delta |
| --- | ---: | ---: | ---: | ---: |
| Default | 7.660 | 0% | 7.516 | 0% |
| None | 5.809 | +72.09% | 5.740 | +71.65% |
| Sub | 7.569 | +1.49% | 7.422 | +1.49% |
| Up | 7.877 | +18.79% | 7.724 | +18.86% |
| Average | 7.830 | +6.93% | 7.694 | +7.04% |
| Paeth | 8.188 | +3.84% | 8.054 | +3.53% |
| Adaptive | 7.667 | 0% | 7.523 | 0% |

Default sizes: hard 160,567,130 bytes; feather 154,557,185 bytes.
Sampled encoding-phase working set was approximately 503 MiB across all filters,
with less than 1 MiB variation; private commit was approximately 496 MiB. These
numbers include the benchmark's resident RGB source, not the production GPU scene.

Default/Adaptive were repeated. Clean three-sample medians (initial sweep plus
confirmation iterations 2 and 3) were 7.670/7.667 seconds for hard and
7.516/7.521 seconds for feather. Confirmation iteration 1 was excluded because it
overlapped a test build. There is no meaningful speed difference.

## Reproduction

Build the `pano_png_filter_probe` target, then run
`stitcher/native/tests/windows-png-filter-benchmark.ps1` with `-InputDirectory`,
`-ArtifactDirectory`, and a fresh `-Label`. The input directory must contain
`lifetime-png-hard-1.png` and `lifetime-png-feather-1.png`. Optional `-Filters`
accepts numeric WIC filter values 0 through 6; `-Iterations` controls repetitions.
The script refuses to overwrite existing result files.

Local evidence is under `C:\dev\pano-stitch-native-stage\png-filter-bench`,
labels `sweep` and `confirm`. The experimental regression tests additionally covered
all seven filters with RGB and grayscale PNGs, scanline writes, encoding metadata,
and invalid filter values; those tests were reverted with the filter option.

Microsoft documents [the filter enumeration](https://learn.microsoft.com/en-us/windows/win32/api/wincodec/ne-wincodec-wicpngfilteroption)
and [WIC PNG encoder options](https://learn.microsoft.com/en-us/windows/win32/wic/png-format-overview).
Default is allowed to choose a filtering strategy; matching Adaptive on these files
does not prove it does so for every image or Windows version.

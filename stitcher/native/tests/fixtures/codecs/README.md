# Native codec contract fixtures

These files are frozen synthetic test data. They contain no game captures or third-party images and
may be redistributed with the repository. The fixture set contains:

- an 8-bit RGB PNG without HDR signaling;
- a 16-bit RGB PNG with PNG `cICP` bytes `(9, 16, 0, 1)`, meaning BT.2020 primaries, PQ transfer,
  matrix coefficients identity/RGB, and full range;
- a quality-95, non-progressive, non-optimized 4:4:4 RGB JPEG;
- a three-channel float32, PIZ-compressed scanline EXR containing finite negative and above-one
  samples.

`expected.json` freezes the encoded SHA-256 values, dimensions, decoded samples, and application
color metadata. A deliberate fixture replacement must update binaries and expectations together
and pass the native application contracts.

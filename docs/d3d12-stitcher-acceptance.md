# D3D12 stitcher acceptance

## Transitional archive candidate

- Version: `1.0.4`
- Archive: `PanoramaCapture-Stitcher-1.0.4-win-x64.zip`
- SHA-256: `20878f3155f86c5c55174b4c8409fa449aad0740318a7e268f0b1abf23756c7a`
- Compressed bytes: `74,932,826`
- Extracted bytes: `190,702,305`
- Executable SHA-256: `0d61e06d645a016a84d630c6e5c076e9647934f2d6d8123377de48173bd35bac`
- Native DLL SHA-256: `67746bbb1bc3f18e64e02eca2de3af9817411d805a586b6fbcfe10b3afe370ec`
- Native ABI: `10`

Two clean builds using fixed Python build inputs, MSVC `/Brepro`, sorted archive entries, and fixed
entry timestamps produced the identical archive SHA-256 above.

The automated artifact audit passed from an extracted path containing spaces. It found exactly one
application executable and one `pano_gpu.dll`, no loose HLSL/CSO/PDB files, no vendor compute
runtime or runtime shader compiler, and no forbidden PE dependency. The audit report records every
HLSL source hash and the candidate's PE dependencies. Corrupting only the extracted DLL produced
the required runtime-failure exit category `3`.

## Completed physical-hardware acceptance

- Date: 2026-08-29
- OS: Windows 11 `10.0.26200`
- Adapter: NVIDIA GeForce RTX 5090
- Vendor/device: `0x10de` / `0x2b85`
- Adapter LUID: `0x0000000000014331`
- Session: `1787897185-2`, 30 SDR PNG frames at 3840×2160
- Runtime verification: ABI load, product adapter probe, embedded pipeline creation, tiny dispatch,
  readback validation, and cleanup passed.
- Resident rendering: preview and 1024-wide hard panorama passed with projected thumbnail and
  coverage output.
- Other output paths: 512-wide feather JPEG and linear EXR passed.
- Forced banding: the first feasible constrained budget was 6528 MiB; a 4096-wide panorama passed
  using 32-row bands.
- Cancellation: cancellation after the first completed D3D12 source upload raised the cancellation
  category, published no output, and left no partial file.
- Previously completed manual GUI acceptance on this adapter covers retained preview interaction,
  hard/feather rendering, exposure correction, thumbnail projection, coverage, error recovery, and
  preview-to-full behavior.

The locally generated acceptance outputs were hashed for the run and then deleted. Captures and
existing user outputs were not modified.

## Headless D3D12 resource regression

- Date: 2026-08-29
- Adapter/session: the Windows 11 RTX 5090 and 30-frame 3840×2160 16-bit PNG session above.
- Isolation: the test-only native executable called the production preview, exposure, and render
  coordinator directly. It created no HWND, GUI thread, or message loop.
- Limits: physical working set below 1,000,000,000 decimal bytes (953 MiB monitor threshold),
  dedicated GPU memory below 2560 MiB, stable preview idle below 5% normalized CPU and GPU, and
  active normalized CPU below 25%. The D3D12 admission budget was 7168 MiB so the conservative
  planner could select its bounded natural-resolution bands; this was not an allocation target.

| Workload | Peak physical RAM | Peak private commit | Peak dedicated/shared VRAM | Peak CPU/GPU | Stable idle CPU/GPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| 3072×1536 retained preview, 4096×2048 hard output | 116.2 MiB | 1655.2 MiB | 1568.8 / 26.2 MiB | 7.1% / 0.1% | 0.04% / 0% |
| 17552×8776 hard, auto exposure, thumbnail, coverage | 116.3 MiB | 2313.9 MiB | 2175.4 / 39.1 MiB | 11.1% / 24.0% | 0% / 0% |
| 17552×8776 feather, auto exposure, thumbnail, coverage | 116.4 MiB | 2487.0 MiB | 2298.4 / 39.1 MiB | 11.1% / 29.3% | 0% / 0% |

The natural hard run spent 42.1 seconds in exposure analysis and 23.0 seconds in rendering. The
natural feather run spent 36.5 seconds in exposure analysis and 20.4 seconds in rendering. Both
published the panorama, projected thumbnail, and coverage output before the monitor verified their
existence and removed them. Native live counts returned to their pre-run values, no probe process
remained, and no existing capture or output was touched.

Windows process private commit includes committed D3D12 allocations and therefore tracks the VRAM
rise; it is not evidence that the same amount is physically resident in host RAM. Physical working
set stayed near 116 MiB in every isolated run. Per-process GPU-engine counters are interval samples,
so active percentages characterize the observed load rather than a sub-interval hardware peak.

These results clear the D3D12 backend itself against the repository's memory-bounded requirement.
The reported idle allocation, CPU/GPU activity, render latency, and 4–12 GiB waves belonged to the
superseded provisional native GUI. The replacement shell's separate retained-preview result is
recorded below.

### Corrected 4096 MiB application-cap acceptance

After separating the user allocation cap from the adapter safety reserve, the same natural hard
workload was admitted under an explicit 4096 MiB cap. It published the 17552×8776 panorama,
thumbnail, and coverage outputs successfully. Peak physical RAM was 129.3 MiB, private commit was
3818.9 MiB, dedicated/shared VRAM was 3399.6/64.8 MiB, normalized CPU was 11.7%, sampled active GPU
was 30.6%, and seven stable idle samples peaked at 0.06% CPU and 0% GPU. Exposure analysis took
36.7 seconds and rendering took 9.0 seconds.

The resource monitor reported failure against its earlier 2560 MiB dedicated-VRAM and 3584 MiB
private-commit comparison ceilings. That result is expected: the corrected planner may select a
larger band up to the explicit 4096 MiB application cap, improving render time from the earlier
23.0-second hard run. The product gate is the selected application cap plus the independent
sub-1,000,000,000-byte physical-host-RAM limit; both passed. Generated outputs were checked and
removed, and no probe process or Windows artifact remained.

## Redesigned native GUI retained-preview regression

- Date/adapter/session: 2026-08-29, the Windows 11 RTX 5090 and 30-frame session above.
- Ownership: all controls, settings, operation queues and workers, cancellation, retained source
  session, preview device/surface, and interaction state are owned beneath the per-window state in
  `GWLP_USERDATA`; the remaining namespace pointer is non-owning and lifetime-bounded.
- A 30-second retained-preview hold produced ten valid samples. Peak physical working set was
  95.684 MiB, private commit was 1711.820 MiB, dedicated/shared VRAM was 1583.973/26.199 MiB, and
  sampled stable-idle CPU/GPU were both 0%. The ignored local evidence is
  `.local/ui-owner-preview-idle-passing.csv`.
- Hidden teardown tests prove every worker/result collection and native D3D12 handle is cleared and
  global native live counts return to zero. MSVC Release `/W4 /WX` and all seven Windows native
  CTests pass after the ownership and accessibility work.
- Native MSAA reports correct standard-control roles and focusable state, while dynamic session
  rows expose local label, pose count, status, and optional tag. The managed UI Automation bridge
  on this host misclassifies all standard Win32 controls as unfocusable Panes; final accessibility
  acceptance must therefore use a real screen reader or Accessibility Insights.

This clears the redesigned frontend's automated retained-resource gate. The combined manual
visual, keyboard, modal, cancellation/error-recovery, and accessibility inspection remains pending
before CPU wire-back.

## Deferred cross-device acceptance checklist (non-blocking)

The completed Windows 11/NVIDIA acceptance above is the physical-hardware gate for the current
D3D12 migration milestone. Complete the broader matrix when suitable systems and capture sessions
become available; these unchecked items do not block Step 16 or subsequent native-application work.

- [ ] Windows 10 candidate startup and rendering.
- [ ] AMD hardware adapter.
- [ ] Intel Arc or supported integrated hardware adapter.
- [ ] Clean CPU-fallback-only machine with no compatible adapter.
- [ ] Known JPEG-input, 16-bit Rec.2020/PQ PNG-input, and float EXR-input capture sessions across
  the available physical-hardware matrix.
- [ ] Exact candidate GUI inspection on each target, including preview interaction and
  cancellation.

No AMD/Intel/CPU-only machine or known PQ/EXR capture session was available during the local run.
WARP and synthetic tests remain required regression gates but are not recorded as physical-hardware
acceptance.

## Native comparison candidate

- Date: 2026-08-29
- Archive: `PanoramaCapture-Stitcher-1.0.4-comparison-win-x64.zip`
- Path: `C:\dev\panorama-step22-comparison-r3-dist`
- SHA-256: `61fb9fdd66df4b17021fb79fd222fc5844801511dcf827f20d6341a6de9618bf`
- Compressed bytes: `75,408,515`
- Extracted bytes: `191,895,583`
- Native executable SHA-256: `05b19a19cb9674b0ffb3015fc465624a9aa7784af4fbcaa5a4a740a90c0c1c7c`
- Python executable SHA-256: `7c872afc32f00a15e4f4e402c901a2d1d492d650304b98d5af87d35ca43fe850`
- Shared native DLL SHA-256: `41e7b692c474c15559b4c16c2af4470e7094daeb2adee48de2175e50f0d8c18a`

The automated comparison audit passed from an extracted path containing spaces. Both exact entry
points loaded the one root `pano_gpu.dll`, reported ABI 10 and the same RTX 5090 vendor/device/LUID,
and returned runtime-failure exit 3 when that DLL was replaced temporarily with invalid bytes. The
audit found no vendor compute runtime, runtime shader compiler, loose shader, or forbidden PE
dependency, and restored/removed all temporary audit files.

After explicit probe-path quoting was added, the comparison audit was repeated. Both entry points
again reported ABI 10 and the same RTX 5090 identity, both corrupt-DLL checks completed through the
controlled runtime-failure path, and the post-run process audit found no stitcher, native-test, or
Windows Error Reporting process. The temporary report, extraction, and probe diagnostic were
removed.

The remaining Step 22a.2 gate is a manual Windows 11 interaction/accessibility comparison of the
two entry points. In particular, verify native session discovery and history labels, keyboard tab
order/activation, preview selection and exposure controls, render/cancel/error recovery, persisted
settings after restart, and both deletion confirmation choices using disposable session copies.
Do not make the native entry point the release default or remove the Python rollback entry until
this comparison has no release-blocking difference.

## Native-only explicit candidate

- Date: 2026-08-29
- Archive: `PanoramaCapture-Stitcher-1.0.4-native-candidate-win-x64.zip`
- Path: `C:\dev\panorama-step22-native-candidate-r3-dist`
- SHA-256: `8f7c4dd1a53c8b63b2635741fe6567a7ece4bac8fbfef52957ce6b76124b84ae`
- Compressed bytes: `914,977`
- Extracted bytes: `2,198,465`
- Executable SHA-256: `0b953d809d7a3b246b66323e414d596e0e36ded7794d6273d94828a599c13945`
- Native DLL SHA-256: `bf9ce4b8eef5b1b5bebaa6b98b60c23499776cdcea300d3b9e27a9922c19b1e9`

Two clean explicit-native builds produced byte-identical archives. The build path did not create a
Python environment or invoke pip/PyInstaller. The strict extracted-path audit found one executable,
one native DLL, notices, and README; no Python/Tk, NumPy, OpenCV, Pillow, Python OpenEXR binding,
loose shader, compiler, vendor runtime, or MSVC redistributable payload/dependency. The executable
and DLL statically link the MSVC runtime and import only Windows system libraries from the host.
The RTX 5090 ABI/pipeline probe and corrupt-DLL exit-3 check passed.

The same retained archive was also cleanly extracted to
`C:\dev\Panorama Native Ω 空格`; its quoted-path runtime probe returned ABI 10 and the same RTX
5090 vendor/device/LUID, and the temporary extraction was removed. This exposed and fixed an audit
invocation defect: PowerShell `Start-Process -ArgumentList` had received the probe-result path
without explicit quotes, so a path containing spaces could be split and launch the GUI instead of
the headless probe. The corrected strict native-only audit passed both the normal runtime probe and
corrupt-DLL exit-3 check from `Panorama Capture Release Audit`, then removed its extraction and
diagnostic files.

The native settings loader uses the same `%APPDATA%\PanoramaCapture\gui-settings.json` path and
field names as the Python frontend. Its MSVC contract test now loads a Python-shaped valid
`stitched_sessions` entry, checks paths, history output, and auto-contrast, then passes the native
mutation/save/reload checks. Manual GUI restart remains required to confirm the migrated values are
presented and persisted correctly by the controls.

This candidate is not yet the default release. Its remaining manual gate is the same Step 22a.2
interaction/accessibility comparison described above; use the comparison archive for rollback.

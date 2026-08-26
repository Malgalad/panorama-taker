# AGENTS.md

## Interaction style

- Before editing, review the requested change against the existing implementation and summarize
  the intended edits. If requirements conflict, logic appears incorrect, or the necessary scope
  is materially wider than requested, pause and ask for clarification or approval.
- Before every file-editing operation, give a short explanation of what will be edited and why.
- In the final response, briefly explain each modified file and why the change was required.

## Repository overview

This repository captures and stitches full-sphere panoramas from Cyberpunk 2077. It contains three
cooperating runtime components plus their shared metadata contract:

- `mod/cet/PanoramaCaptureProbe/init.lua`: production CET Lua capture controller.
- `reshade-addon/`: Windows x64 C++ ReShade screenshot add-on using vendored ReShade API headers.
- `stitcher/`: Python 3.12 panorama compositor, CLI, and Tkinter GUI.
- `contracts/`: shared session schema and example metadata.
- `docs/`: design decisions, research, and implementation specifications.
- `release/`: Windows release build scripts.

Read the relevant README, tests, and focused document under `docs/` before changing a component.
Treat current code and tests as authoritative when an older plan disagrees with implemented
behavior. `PLAN.md` contains useful architectural context, but some sections describe historical
or proposed designs rather than the current repository.

## General change rules

- Keep changes narrow. Do not refactor adjacent code merely because it could be cleaner.
- Prefer the smallest implementation that preserves existing public behavior and contracts.
- Do not change capture metadata/schema, output color behavior, projection conventions, file
  naming, defaults, or session-history format unless the request explicitly requires it.
- Preserve user changes in a dirty working tree. Never discard or rewrite unrelated modifications.
- Use existing named types and helpers before introducing new abstractions. In typed code, avoid
  ad hoc unions/intersections or type-expression drilling when an existing named type fits.
- Add comments only for non-obvious invariants and edge cases. Do not narrate straightforward code.
- Match surrounding style and naming. Keep Python lines within the configured 100-column limit.
- Update or add focused tests for behavior changes. A bug fix should normally include a regression
  test.
- Do not claim a check passed unless it was actually run. Report any check that could not run and
  why.
- Never deploy files into the game installation, launch the game, delete captures, or overwrite
  user outputs unless the user explicitly asks.

## Stitcher (`stitcher/`)

The stitcher is Python 3.12 and uses NumPy, OpenCV, Pillow, OpenEXR, jsonschema, pytest, Ruff, and
strict mypy. Source code lives in `stitcher/src/pano_stitch`; tests live in `stitcher/tests`.

Important invariants:

- Projection uses recorded camera geometry; do not introduce feature matching as an implicit
  fallback.
- Preserve HDR data and the existing Rec.2020/PQ, linear, SDR, and EXR conversion contracts.
- Rendering is memory-bounded and strip-based. Do not add full-resolution in-memory copies when a
  strip, memmap, or streaming writer can be used.
- Maintain cooperative cancellation, temporary-file cleanup, and write-then-replace output
  behavior.
- Keep GUI image work off the Tk main thread. GUI widgets must only be updated through the existing
  event-queue pattern.
- Keep CLI and GUI behavior aligned when a render option is useful to both.
- Prefer keyword arguments for optional render settings, especially adjacent Boolean options.

Run checks from `stitcher/`:

```text
uv run ruff check .
uv run ruff format --check .
uv run mypy src
uv run pytest
```

During iteration, focused tests are acceptable, but run the full applicable suite before handoff.
If formatting changes are required, run `uv run ruff format .`, then rerun the checks above.

## CET mod (`mod/cet/`)

The production capture controller is Lua executed by Cyber Engine Tweaks. Preserve these rules:

- Every capture-side state change must have a reliable restoration path for success, abort,
  shutdown, and recoverable failure.
- Derive each camera pose from the saved origin; avoid cumulative rotation drift.
- A frame is not captured until the ReShade bridge reports the matching completed screenshot.
- Keep request/ack writes atomic and correlation-token checks strict.
- Do not block CET's update thread or replace real update-frame settling with presentation-frame
  guesses.
- Keep optional mod integrations optional unless requirements explicitly change.

There is no repository-configured Lua linter. Review syntax carefully and run any available local
Lua parser/checker when practical. Runtime validation in Cyberpunk is a separate manual step and
must be reported as such.

## ReShade add-on (`reshade-addon/`)

The add-on is Windows x64 C++17 and targets the vendored ReShade v6.7.3 API 18 headers. Do not
silently replace those headers with upstream `main`.

Important invariants:

- Invoke ReShade runtime methods only from documented ReShade callback contexts, not IPC worker
  threads.
- Match screenshot completion by both runtime and correlation token.
- Ignore unrelated screenshots and reject concurrent requests.
- Preserve timeout recovery and atomic bridge response behavior.
- Build with warnings treated as errors.

The native build requires a Windows x64 MSVC environment. Use the documented CMake commands in
`reshade-addon/README.md`. On non-Windows hosts, run the Python contract tests where available:

```text
python -m pytest reshade-addon/tests
```

Do not describe a Linux-only contract-test pass as a successful native add-on build.

## Native fallback plugin (`mod/`)

`mod/src/plugin.cpp` is a C++20 RED4ext fallback/probe, separate from the production CET Lua path.
Do not expand or switch to this backend without explicit approval. Its MSVC build uses `/W4 /WX`
and requires the RED4ext SDK configured by `mod/CMakeLists.txt`.

## Contracts and documentation

- Changes to `contracts/session.schema.json` must remain compatible with both metadata loaders and
  capture writers, or include an explicit version/migration decision.
- Keep `contracts/example-session.json` and metadata tests synchronized with intentional schema
  changes.
- Documentation should describe verified current behavior. Clearly label proposals, assumptions,
  and manual validation steps.
- When implementing a focused design document, follow its fixed behavior unless current code makes
  it unsafe or contradictory; report that conflict before proceeding.

## Completion checklist

Before declaring work complete:

1. Review the diff for accidental scope expansion and unrelated formatting churn.
2. Run the narrowest relevant tests plus all feasible component-level lint, format, type, and test
   checks.
3. Confirm temporary files, generated binaries, caches, and user outputs were not added to the
   change accidentally.
4. Summarize modified files, behavior, checks run, and any remaining manual validation.

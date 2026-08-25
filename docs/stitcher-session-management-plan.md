## Implementation checklist

## 1. Session model and discovery

- [ ] Add a small testable session-catalog module, rather than stuffing more filesystem logic into [gui.py](/home/pogorelov/panorama-taker/stitcher/src/pano_stitch/gui.py).
- [ ] Derive the mod directory from:
  `GAME_DIR/bin/x64/plugins/cyber_engine_tweaks/mods/PanoramaCaptureProbe`
- [ ] Discover only `PanoramaCaptureBridge.pano-*.json`; exclude `settings.json`, temporary files, and bridge protocol files.
- [ ] Load each session through the existing metadata loader.
- [ ] Represent each row with JSON path, session ID, image paths, completion status, and stitch history.
- [ ] Parse the leading epoch from `<epoch>-<counter>` and format it using the computer’s local timezone.
- [ ] Sort sessions newest first.
- [ ] Report unreadable/malformed session files without crashing the entire list.

## 2. Replace the capture input

- [ ] Replace “Capture JSON” with a “Game directory” folder picker.
- [ ] Persist `game_dir` in `gui-settings.json`; stop using `session_dir`.
- [ ] Validate that the derived mod directory exists and show an actionable error otherwise.
- [ ] Add a session list with columns:
  - Local date
  - Complete / Incomplete
  - Stitched / Not stitched
- [ ] Refresh automatically when the game directory changes.
- [ ] Add a manual Refresh button.
- [ ] Clear validation and disable Render when no session is selected.
- [ ] On selection, set the internal JSON path and infer the screenshots directory as today.

Assumption: retain the Screenshots field as an override for moved captures. Normal game-directory use will populate it automatically.

## 3. Preserve stitch history and output name

- [ ] Extend `gui-settings.json` with stitch history keyed by normalized game directory plus session ID.
- [ ] Store the exact final output filename only after a successful render.
- [ ] Mark the session “Stitched” immediately after success and refresh its row.
- [ ] When reselecting a stitched session, restore its last successful output filename and matching format.
- [ ] Do not replace the preserved name with a generated default unless the user explicitly edits it.
- [ ] Keep history when source files are deleted so the retained panorama remains recorded.
- [ ] Never infer “stitched” merely from a similarly named file in the output directory.

## 4. Session management

- [ ] Add “Delete JSON” for every session.
- [ ] Add “Delete JSON and captured images.”
- [ ] For stitched sessions, label source cleanup explicitly as “Delete JSON and screenshots, keep panorama.”
- [ ] Delete only the selected JSON and image files explicitly referenced by it—never recursively delete a directory.
- [ ] Deduplicate image paths and tolerate already-missing files.
- [ ] Never delete panorama or coverage outputs during source cleanup.
- [ ] Disable deletion while validation/rendering is active.
- [ ] After deletion, refresh the list, clear the removed selection, and report deleted/missing file counts.

Confirmation rules:

- [ ] Skip confirmation only for “Delete JSON” on an incomplete session.
- [ ] Confirm every other deletion.
- [ ] Include the exact text `Are you sure?` and summarize what will be removed.
- [ ] Perform no deletion if confirmation is declined.

## 5. Tests and acceptance

- [ ] Test game-directory-to-mod-directory resolution.
- [ ] Test discovery filtering and newest-first ordering.
- [ ] Test epoch-to-local-date conversion.
- [ ] Test complete/incomplete classification.
- [ ] Test stitch-history serialization and output-name restoration.
- [ ] Test every confirmation-rule combination.
- [ ] Test deletion leaves unrelated files and stitched panoramas untouched.
- [ ] Test missing/malformed JSON and missing screenshots.
- [ ] Run `ruff check`, `mypy`, and `pytest`.
- [ ] Perform a native Windows smoke test covering selection, refresh, rendering, persistence, and all deletion actions.

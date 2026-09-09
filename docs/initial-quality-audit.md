# Initial quality audit — 2026-09-09

This audit establishes the starting point for `v0.1.0-internal`. It is not a
release-blocker waiver; each item needs an issue and a targeted fix before a
strict warning-free release is claimed.

## Verified baseline

- `make test` passes with synthetic parser and numerical fixtures.
- `make` builds the SDL application on the development macOS environment.
- `git diff --check` reports no whitespace errors.

## Known technical debt

| Area | Finding | Priority |
| --- | --- | --- |
| Peak finder | The exposed `noise_pts` / Noise Window parameter is not used by `run_peak_finder`. | High: scientific correctness |
| Event handling | Toolbar rendering and hit testing define different toolbar Y origins. | High: interaction correctness |
| Initialization | Several UI structs rely on omitted trailing initializer fields. | Medium: warning-free build |
| Portability | Some headers/source files lack final newlines; the app tries a user-specific font path first. | Medium: release hygiene |
| Architecture | Multi-spectrum state mirrors active data into legacy `AppState` fields. | Medium: migration risk |
| Exports | Fixed current-directory filenames can overwrite earlier outputs. | Medium: data integrity |

## Policy for this milestone

The items above are intentionally documented rather than changed incidentally.
Each fix must have an acceptance criterion and a regression test where
practical. The priority order is scientific correctness, interaction
correctness, data integrity, then code hygiene.

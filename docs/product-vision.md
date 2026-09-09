# Product vision

## Product statement

SpectraVisual is a desktop application for interactive analysis of rotational
spectra. It overlays experimental traces with Pickett SPCAT predictions and
supports rapid keyboard-driven inspection and assignment.

## Current scope

The current programme is feature parity and reliability. No new analytical
features are admitted until the existing SDL implementation has a tested,
documented behavioural baseline and a replacement interface preserves it.

## Non-negotiable user workflows

- Keyboard-first navigation and analysis.
- Simultaneous but distinct experimental and predicted views.
- Explicit `Sync` and independent-view behaviour.
- Fast selection, measurement, and creation of assignments.
- Repeatable imports, analysis settings, and exports.

## Quality bar

An analysis result must be reproducible from recorded inputs and settings.
Unexpected input, failed import, truncated data, or failed export must result
in an actionable user-facing error rather than silent loss or mutation.

## Migration direction

The existing scientific core remains C. A future Qt/C++ application layer will
replace SDL only after core parity is demonstrated by automated and human
validation. This is an architectural direction, not an authorization to add
new features during the parity milestone.

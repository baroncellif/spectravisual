# Keyboard and dual-view contract

This document is the compatibility contract for all user-interface work. The
keyboard mapping is a primary workflow, not an optional convenience.

## Dual-view invariants

- Experimental data and prediction data retain independent ranges and state.
- `Sync` links the horizontal visible range of the two views; it never merges
  their data, vertical scales, selections, or analysis state.
- With `Sync` disabled, an unmodified navigation key targets the experimental
  view and the corresponding `Shift` chord targets the prediction view.
- The experimental offset is a display/alignment operation; stored experimental
  frequencies remain in their true coordinate system.
- The application must visibly communicate whether `Sync` is active.

## Required mappings

| Input | Sync active | Sync disabled |
| --- | --- | --- |
| `A` / `S` | Pan both views left / right | Pan experiment; `Shift` pans prediction |
| `Q` / `E` | Zoom both views out / in | Zoom experiment; `Shift` zooms prediction |
| `W` / `Z` | Change experimental intensity scale | Change experiment; `Shift` scales prediction |
| Up / Down | Move experimental vertical view | Move experiment; `Shift` scales prediction |
| `K` / `L` | Move both cursor bars | Move experiment bar; `Shift` moves prediction bar |
| `Tab` | Autoscale the current experimental display | Same |
| `R` | Reset view state | Same |
| `G` | Toggle two-click distance measurement | Same |
| `N`, `P`, `T`, `M` | Assignments, peak finder, average, broadening | Same |
| `C`, `F`, `B` | Intensity cut, frequency jump, filter | Same |
| `X`, `H` / `?` | Export; help | Same |
| Delete / Backspace | Remove latest peak; `Shift` clears peak list | Same |

## Pointer mappings

- Left-drag on the experimental view selects a frequency interval and zooms.
- Right-drag on the experimental view performs peak selection/search.
- Option/Alt + left-drag shifts the experimental trace horizontally for
  alignment, without altering stored frequencies.
- Clicking prediction lines selects transitions; modifier-click supports
  multiple selection.

## Acceptance protocol

Every pull request touching event handling, layout, or visualization must be
checked in both `Sync` states. For each affected mapping, record: starting
range, input, resulting experimental range, resulting prediction range,
selection state, and observed screen feedback.

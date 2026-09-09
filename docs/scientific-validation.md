# Scientific validation policy

## Purpose

The program is research software. A successful build is necessary but never
sufficient evidence that an analytical result is correct.

## Regression levels

1. **Unit tests** verify deterministic numerical functions and parsers using
   small, synthetic fixtures.
2. **Golden-result tests** compare documented frequencies, assignments, filter
   results, and broadened values against approved reference outputs.
3. **Human validation** checks plotted alignment, interactive selection, and
   keyboard-only workflow on representative spectra.

## Required evidence by change type

| Change | Minimum evidence |
| --- | --- |
| Parser or file format | Fixture plus expected parsed fields and error cases |
| Peak finder or smoothing | Numerical unit test and reference-spectrum review |
| Broadening or filtering | Reference values plus visual comparison |
| Assignment handling | Round-trip save/load test and scientist review |
| Keyboard or dual view | Contract checklist in both Sync states |
| Build/release tooling | Clean build in CI on supported platforms |

## Data handling

Do not commit unpublished spectra. Fixtures must be synthetic, minimal, and
documented. Reference outputs derived from real data require explicit approval
before entering this repository.

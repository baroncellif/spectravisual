# Internal contribution workflow

SpectraVisual is a private proprietary project. Access to the repository does
not grant permission to redistribute its source, data, binaries, or outputs.

## Working agreement

1. Start from an issue with a concrete scientific or engineering outcome.
2. Work on a short-lived branch named `feature/...`, `fix/...`, `test/...`, or
   `chore/...`; never develop directly on `main`.
3. Open a pull request that explains the behavioural change, its scientific
   impact, and how it was verified.
4. CI must pass. Changes to interaction must satisfy the keyboard and
   dual-view contract in `docs/keyboard-contract.md`.
5. The product owner performs the human checkpoint on representative spectra
   before merge.

## Definition of done

A change is ready to merge only when it has:

- a linked issue or documented rationale;
- focused commits and no unrelated formatting churn;
- relevant automated tests or an explicit reason why none are possible;
- updated documentation when behaviour, build, or workflow changes;
- a passing `make test` run locally and in CI;
- a human scientific validation when analysis, assignment, visualization, or
  keyboard behaviour changed.

Never commit real, unpublished spectra or prediction files. Use the synthetic
fixtures in `tests/fixtures/`, or approved non-sensitive data, for regression
tests.

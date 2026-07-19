# Wiring the bench budget gate into CI

This is **not** wired into `.github/workflows/ci.yml` yet — deliberately,
to avoid stepping on other work touching that file. This document says
exactly what to add, so a human (or a later pass) can do it without
guessing intent.

## What to add, and where

The existing job in `.github/workflows/ci.yml` is `build-test`, matrixed
over `os: [ubuntu-latest, macos-latest]`, ending in a `Test` step that runs
`ctest`. Add three new steps to the **same job**, immediately after the
existing `Test` step (same matrix, so the benchmark runs on both Linux and
macOS — the ratio check is machine-relative so this is fine, and the drift
history is kept separate per-OS anyway):

```yaml
      - name: Test
        run: ctest --test-dir build --output-on-failure

      - name: Build bench
        run: cmake --build build --parallel --target bench

      - name: Run benchmark
        run: ./build/bench

      - name: Check performance budget
        run: python3 bench/check_budget.py check
```

`check_budget.py` needs no dependencies beyond the Python 3 standard
library, and both `ubuntu-latest` and `macos-latest` GitHub-hosted runners
ship Python 3 preinstalled, so no setup step is required.

This is enough to make the gate hard-fail the build on a real regression
(check 1) and print drift warnings (check 2) on every push and PR. `python3
bench/check_budget.py check` exits non-zero — and therefore fails the step
— only when the LadderBook/OrderBook ratio budget is violated; drift
warnings print to the step's log but never fail it.

## Recording history (main only)

Check 2 needs `bench/ci_history.csv` to accumulate real CI numbers over
time. That should only happen for runs that actually merged to `main` (not
every PR build, which could be from an unmerged or since-abandoned branch,
and would pollute the history with numbers that never shipped). Add one
more step, gated on the same job, after the budget check:

```yaml
      - name: Record benchmark history
        if: github.ref == 'refs/heads/main' && github.event_name == 'push'
        run: python3 bench/check_budget.py record

      - name: Commit updated history
        if: github.ref == 'refs/heads/main' && github.event_name == 'push'
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add bench/ci_history.csv
          git diff --staged --quiet || git commit -m "bench: record CI history for ${GITHUB_SHA::12}"
          git diff --staged --quiet || git push
```

Notes for whoever wires this up:

- `record` writes to the working tree's `bench/ci_history.csv`; the commit
  step above is what persists it back to `main`. If committing bot-authored
  changes back to `main` from CI is against this repo's policies, an
  alternative is uploading `bench/ci_history.csv` as a build artifact per
  run and having a separate scheduled job merge them — more moving parts,
  only worth it if direct pushes from CI are unwanted.
- Because the matrix runs both `ubuntu-latest` and `macos-latest` in
  parallel, both `Commit updated history` steps race to push. `record` is
  idempotent per `(commit_sha, os)`, so a `git pull --rebase` before the
  push (or just accepting that the second push needs a retry after pulling)
  avoids either job clobbering the other's row. A minimal safe version:

  ```yaml
      - name: Commit updated history
        if: github.ref == 'refs/heads/main' && github.event_name == 'push'
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          git add bench/ci_history.csv
          git diff --staged --quiet && exit 0
          git commit -m "bench: record CI history for ${GITHUB_SHA::12} (${{ matrix.os }})"
          for i in 1 2 3; do
            git pull --rebase origin main && git push && break
            sleep $((RANDOM % 5 + 1))
          done
      permissions:
        contents: write
  ```

  (`permissions: contents: write` is needed on the job or workflow for the
  push to succeed with the default `GITHUB_TOKEN`.)
- If that push-from-CI complexity isn't wanted at all, the simplest
  alternative is to drop the `record`/commit steps entirely and instead run
  `python3 bench/check_budget.py record` locally after merging a
  performance-sensitive change, committing the updated
  `bench/ci_history.csv` by hand. Check 1 (the actual gate) works
  identically either way — check 2 just has thinner history to compare
  against.

## What NOT to change

Per the task this integration doc accompanies: do not edit
`.github/workflows/ci.yml`, `CMakeLists.txt`, or the top-level `README.md`
as part of landing `bench/check_budget.py` — those are intentionally left
for a human (or separate change) to wire up using the exact steps above.

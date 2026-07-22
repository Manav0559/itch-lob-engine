# Wiring the bench budget gate into CI

This **is** wired into `.github/workflows/ci.yml`, in the `build-test` job
(matrixed over `os: [ubuntu-latest, macos-latest]`). This document says
exactly what's there and why, so a later change to `ci.yml` doesn't have to
reverse-engineer intent from the workflow file alone.

## What's there, and where

Immediately after the `Test` step (`ctest`), the same job builds and runs the
benchmark and gates on it:

```yaml
      - name: Build bench
        run: cmake --build build --parallel --target bench

      - name: Run benchmark and check performance budget
        run: |
          for attempt in 1 2 3; do
            ./build/bench
            if python3 bench/check_budget.py check; then
              exit 0
            fi
          done
          exit 1
```

(see `ci.yml` for the full comment explaining the up-to-3-attempts retry —
it exists to absorb single-run CI noise on the tightest-margin message type,
not to paper over a real regression.)

`check_budget.py` needs no dependencies beyond the Python 3 standard
library, and both `ubuntu-latest` and `macos-latest` GitHub-hosted runners
ship Python 3 preinstalled, so no setup step is required.

This alone is enough to make the gate hard-fail the build on a real
regression (check 1) and print drift warnings (check 2) on every push and
PR. `python3 bench/check_budget.py check` exits non-zero — and therefore
fails the step — only when the LadderBook/OrderBook ratio budget is
violated; drift warnings print to the step's log but never fail it.

## Recording history (main only) — now automated

Check 2 needs `bench/ci_history.csv` to accumulate real CI numbers over
time. That only happens for runs that actually merged to `main` (not every
PR build, which could be from an unmerged or since-abandoned branch, and
would pollute the history with numbers that never shipped). One more step
runs in the same job, after the budget check, gated to push-to-main only:

```yaml
      - name: Record and commit benchmark history
        if: github.ref == 'refs/heads/main' && github.event_name == 'push'
        run: |
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"
          cp bench/results.csv /tmp/results.csv
          cp bench/results_threaded.csv /tmp/results_threaded.csv
          for i in 1 2 3; do
            cp /tmp/results.csv bench/results.csv
            cp /tmp/results_threaded.csv bench/results_threaded.csv
            python3 bench/check_budget.py record
            git checkout -- bench/results.csv bench/results_threaded.csv
            git add bench/ci_history.csv
            if git diff --staged --quiet; then
              exit 0
            fi
            git commit -m "bench: record CI history for ${GITHUB_SHA::12} (${{ matrix.os }})"
            if git push origin HEAD:main; then
              exit 0
            fi
            git fetch origin main --quiet
            git reset --hard origin/main
            sleep $((RANDOM % 5 + 1))
          done
          exit 1
```

(abbreviated here — see `ci.yml` for the full step with comments.)

Notes on how this actually behaves in production:

- `record` (`python3 bench/check_budget.py record`) writes to the working
  tree's `bench/ci_history.csv`; the same step is what persists it back to
  `main`. This repo's Actions "Workflow permissions" setting is "Read and
  write", and the `build-test` job declares its own
  `permissions: contents: write` block (scoped to that job, not the whole
  workflow, so `sanitize` and `fuzz-smoke` never get a write-capable token
  they have no use for) — so the default `GITHUB_TOKEN` is sufficient for
  the push. No PAT or deploy key is needed.
- The step's `if:` condition (`github.ref == 'refs/heads/main' &&
  github.event_name == 'push'`) must hold for it to run at all. A
  `pull_request` event — including one opened from a fork — never runs it,
  and separately GitHub always forces `GITHUB_TOKEN` to read-only on
  `pull_request` runs regardless of any `permissions` block, so there's no
  path by which a fork PR could push to `main`.
- `git diff --staged --quiet` guards the commit: if `record` was a no-op
  (e.g. a re-triggered run for a commit/os pair already in history — see
  `check_budget.py`'s `record` idempotency), there's nothing staged and the
  step exits 0 without creating an empty commit.
- Because the matrix runs both `ubuntu-latest` and `macos-latest` in
  parallel, both legs race to append their own OS's rows for the same
  commit and push. This is a real, deterministic conflict, not just an
  occasional timing fluke: both legs' commits insert different new rows
  immediately after the exact same last line of the exact same shared
  file, relative to the exact same parent commit -- an unresolvable
  insertion-order ambiguity to *every* git merge strategy (verified: a
  plain `git merge` conflicts here identically to `git pull --rebase`).
  A retry that just re-attempts the git commands can't fix that; the loop
  above instead discards the losing leg's stale commit outright
  (`git fetch` + `git reset --hard origin/main`) and re-runs `record`
  against the now-current file -- since `record` is idempotent and purely
  appends, that produces a clean append with nothing left to reconcile.
  Up to 3 record-commit-push attempts, with a small random backoff between
  them, before giving up and failing the step.
- If committing bot-authored changes back to `main` from CI is ever against
  this repo's policies, the alternative sketched in earlier revisions of
  this document still applies: drop the `record`/commit steps, upload
  `bench/results.csv` as a build artifact per run instead, and either merge
  those artifacts with a separate scheduled job or fall back to running
  `python3 bench/check_budget.py record` locally after a performance-
  sensitive change and committing `bench/ci_history.csv` by hand. Check 1
  (the actual gate) works identically either way — check 2 just has thinner
  history to compare against without the automated recording.

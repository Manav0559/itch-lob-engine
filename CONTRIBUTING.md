# Contributing

This is a solo-maintained portfolio project. PRs and issues are welcome, but review
may take a while — there's no dedicated review SLA.

Start with [docs/architecture.md](docs/architecture.md) to see how the pieces fit
together before changing anything. For build/test/coverage commands, see
[Build & test](README.md#build--test) in the README — this file doesn't duplicate
those.

Two hard rules for any change:

- Perf numbers in the README and dashboard come from a committed benchmark target
  (`bench/`), not hand-measured claims — if you change hot-path code, re-run the
  benchmark and update the committed CSVs rather than eyeballing it.
- CI must stay green: the build matrix, `-Werror`, the sanitizer jobs (ASan/UBSan/TSan),
  and the fuzz smoke test all gate merges. If you add code that touches concurrency or
  parsing, expect the sanitizer/fuzz jobs to actually exercise it.

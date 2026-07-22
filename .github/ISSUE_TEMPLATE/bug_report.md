---
name: Bug report
about: Report incorrect behavior in the parser, order book, or execution layer
title: ''
labels: bug
---

**Describe the bug**
What happened, and what did you expect instead?

**Repro steps**
1. Command line invoked (e.g. `./build/replay --selftest`, or the ITCH file / flags used)
2. Minimal input that triggers it, if not `--selftest`
3. Observed output/crash

**Does it reproduce under `--selftest`?**
`--selftest` runs the pipeline against a synthetic session with no external data file —
if the bug reproduces there, include the exact command; it's the fastest path to a fix.

**Environment**
- OS:
- Compiler + version:
- Build type (Release/Debug, sanitizers enabled?):

**Additional context**
Stack trace, sanitizer output, or anything else relevant.

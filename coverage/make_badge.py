#!/usr/bin/env python3
"""Turn an lcov tracefile into a coverage percentage, a CI job summary, and
a shields.io endpoint badge.

    python3 coverage/make_badge.py coverage/lcov.filtered.info

See coverage/README.md for how this fits into .github/workflows/ci.yml's
`coverage` job. Reads the lcov/geninfo tracefile format directly (the
per-file SF:/LF:/LH:/BRF:/BRH:/end_of_record records) rather than scraping
`lcov --list`'s human-formatted table, so a future lcov version reflowing
that table's columns can't silently break this -- LF/LH/BRF/BRH are lcov's
own per-file totals (lines found/hit, branches found/hit), so summing them
across every SF record in the tracefile is exactly what `lcov --list`
itself reports as the grand total, just without depending on its display
formatting.

Writes:
  - coverage/coverage.json -- shields.io "endpoint" schema
    (https://shields.io/badges/endpoint-badge). CI commits this to `main`
    (push events only -- see the `coverage` job) so the README badge has a
    stable raw.githubusercontent.com URL to point at, the same
    commit-generated-data-back-to-main pattern bench/ci_history.csv already
    uses for the performance-budget history.
  - a markdown table to stdout, meant for `>> "$GITHUB_STEP_SUMMARY"` so
    the number shows up on the Actions run page even without opening the
    README.

Exits non-zero (and skips writing coverage.json) if the tracefile has no
instrumented lines at all -- that's a broken capture upstream (wrong
--directory, no .gcda files, filtered everything out), not real 0%
coverage, and silently overwriting a real badge with a bogus one would be
worse than just failing the step loudly.
"""
import json
import pathlib
import sys


def parse_totals(tracefile):
    lines_found = lines_hit = 0
    branches_found = branches_hit = 0
    with open(tracefile) as f:
        for raw_line in f:
            raw_line = raw_line.strip()
            if raw_line.startswith("LF:"):
                lines_found += int(raw_line[len("LF:"):])
            elif raw_line.startswith("LH:"):
                lines_hit += int(raw_line[len("LH:"):])
            elif raw_line.startswith("BRF:"):
                branches_found += int(raw_line[len("BRF:"):])
            elif raw_line.startswith("BRH:"):
                branches_hit += int(raw_line[len("BRH:"):])
    return lines_found, lines_hit, branches_found, branches_hit


def pct(hit, found):
    return (100.0 * hit / found) if found else 0.0


def badge_color(line_pct):
    # Same rough bands shields.io's own built-in coverage badges use --
    # not a claim about what's "good enough" for this specific codebase,
    # just a legible traffic-light gradient for the README.
    if line_pct >= 90:
        return "brightgreen"
    if line_pct >= 75:
        return "green"
    if line_pct >= 60:
        return "yellow"
    if line_pct >= 40:
        return "orange"
    return "red"


def main(argv):
    if len(argv) != 2:
        print("usage: make_badge.py <lcov-tracefile>", file=sys.stderr)
        return 2

    tracefile = argv[1]
    lines_found, lines_hit, branches_found, branches_hit = parse_totals(tracefile)

    if lines_found == 0:
        print(
            f"error: {tracefile} has zero instrumented lines -- this is a "
            "broken capture (wrong --directory, no .gcda files produced, or "
            "the --remove filters ate everything), not real 0% coverage. "
            "Refusing to publish a badge from it.",
            file=sys.stderr,
        )
        return 1

    line_pct = pct(lines_hit, lines_found)
    branch_pct = pct(branches_hit, branches_found)

    badge = {
        "schemaVersion": 1,
        "label": "coverage",
        "message": f"{line_pct:.1f}% lines / {branch_pct:.1f}% branches",
        "color": badge_color(line_pct),
    }
    out_path = pathlib.Path(__file__).parent / "coverage.json"
    out_path.write_text(json.dumps(badge) + "\n")

    print("## Coverage (include/ + src/, gcc `--coverage` + lcov)")
    print()
    print("| metric | hit | found | % |")
    print("|---|---:|---:|---:|")
    print(f"| lines | {lines_hit} | {lines_found} | {line_pct:.1f}% |")
    print(f"| branches | {branches_hit} | {branches_found} | {branch_pct:.1f}% |")
    print()
    print(f"Wrote `{out_path}` (badge source for the README). Full per-file "
          f"breakdown: `lcov --list {tracefile}` -- see coverage/README.md.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

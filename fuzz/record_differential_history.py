#!/usr/bin/env python3
"""Appends one row to fuzz/differential_history.csv from the last
fuzz_differential run's fuzz/differential_last_run.csv (written by
fuzz_differential.cpp's print_summary() at process exit).

Mirrors bench/check_budget.py's `record` subcommand: same commit-sha /
OS-label resolution (GITHUB_SHA / RUNNER_OS in CI, git/platform.system()
locally), same "append if missing, write header only once" CSV shape.
dashboard/index.html reads this file client-side, same as
bench/ci_history.csv -- this script's only job is turning one committed
run's numbers into one durable row, never computing anything the fuzz
binary itself didn't already measure.

Usage:
    python3 fuzz/record_differential_history.py [--duration-s SECONDS]

Reads fuzz/differential_last_run.csv (must exist -- run
fuzz/build_and_run_differential.sh first) and appends to
fuzz/differential_history.csv, creating it with a header if it doesn't
exist yet.
"""
import argparse
import csv
import os
import platform
import subprocess
from datetime import datetime, timezone

LAST_RUN = "fuzz/differential_last_run.csv"
HISTORY = "fuzz/differential_history.csv"
HISTORY_FIELDS = [
    "timestamp_utc", "commit_sha", "os", "duration_s",
    "inputs", "state_checks", "policy_skips",
]


def detect_sha():
    sha = os.environ.get("GITHUB_SHA")
    if sha:
        return sha[:12]
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip()[:12]
    except Exception:
        return "unknown"


def detect_os_label():
    # RUNNER_OS reports "Linux"/"macOS" in CI; platform.system() reports
    # "Linux"/"Darwin" locally -- same reasoning as
    # bench/check_budget.py's detect_os_label(), kept consistent so a
    # human skimming both CSVs isn't confused by two different labeling
    # schemes for the same platform.
    return os.environ.get("RUNNER_OS") or platform.system() or "unknown"


def read_last_run(path):
    with open(path, newline="") as f:
        row = next(csv.DictReader(f))
    return {k: int(v) for k, v in row.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration-s", type=int, default=None,
                         help="seconds the fuzz run was given (informational only)")
    parser.add_argument("--last-run", default=LAST_RUN)
    parser.add_argument("--history", default=HISTORY)
    args = parser.parse_args()

    last = read_last_run(args.last_run)

    row = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "commit_sha": detect_sha(),
        "os": detect_os_label(),
        "duration_s": args.duration_s if args.duration_s is not None else "",
        "inputs": last["inputs"],
        "state_checks": last["state_checks"],
        "policy_skips": last["policy_skips"],
    }

    file_exists = os.path.exists(args.history) and os.path.getsize(args.history) > 0
    with open(args.history, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=HISTORY_FIELDS)
        if not file_exists:
            writer.writeheader()
        writer.writerow(row)

    print(f"recorded: {row}")


if __name__ == "__main__":
    main()

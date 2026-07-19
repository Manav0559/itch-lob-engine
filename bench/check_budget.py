#!/usr/bin/env python3
"""CI performance-budget gate for the OrderBook vs LadderBook benchmark.

See bench/BUDGET.md for the design rationale (why this is a ratio check
against the same run, not an absolute-nanosecond threshold).

    python3 bench/check_budget.py check          # hard gate + soft warning
    python3 bench/check_budget.py record          # append this run to history

Both subcommands read bench/results.csv (written by ./build/bench) by
default; `record` also appends to bench/ci_history.csv.
"""
import argparse
import csv
import os
import platform
import statistics
import subprocess
import sys
from datetime import datetime, timezone

MESSAGE_TYPES = ["A", "E", "C", "X", "D", "U"]

DEFAULT_RESULTS = "bench/results.csv"
DEFAULT_HISTORY = "bench/ci_history.csv"
HISTORY_FIELDS = [
    "timestamp_utc", "commit_sha", "os", "book_type", "message_type",
    "p50_ns", "p99_ns", "p999_ns",
]


def read_results(path):
    rows = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            key = (row["book_type"], row["message_type"])
            rows[key] = {
                "count": int(row["count"]),
                "p50_ns": int(row["p50_ns"]),
                "p99_ns": int(row["p99_ns"]),
                "p999_ns": int(row["p999_ns"]),
            }
    return rows


def read_history(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def detect_commit_sha():
    sha = os.environ.get("GITHUB_SHA")
    if sha:
        return sha[:12]
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"],
            capture_output=True, text=True, check=True,
        )
        return out.stdout.strip()
    except Exception:
        return "unknown"


def detect_os_label():
    return os.environ.get("RUNNER_OS") or platform.system() or "unknown"


def check_ratio(results, threshold):
    """Hard check: LadderBook p50 must beat OrderBook p50 by `threshold`,
    for every message type present in this run. Ratio-based on the SAME
    run/machine/moment, so it cancels out cross-run and cross-machine
    noise -- this is the check that is allowed to fail CI.
    """
    checked = []
    failures = []
    for mtype in MESSAGE_TYPES:
        order = results.get(("OrderBook", mtype))
        ladder = results.get(("LadderBook", mtype))
        if order is None or ladder is None:
            continue
        limit = threshold * order["p50_ns"]
        ratio = ladder["p50_ns"] / order["p50_ns"] if order["p50_ns"] else 0.0
        entry = (mtype, order["p50_ns"], ladder["p50_ns"], ratio, limit)
        checked.append(entry)
        if ladder["p50_ns"] >= limit:
            failures.append(entry)
    return checked, failures


def check_drift(results, history, os_label, window, multiplier, min_history):
    """Soft check: warn (never fail) if a book's p99 on THIS run drifts far
    past the median of its own recent CI history on the same OS runner.
    Absolute nanosecond numbers on a shared CI runner are too noisy to gate
    on directly, so this is informational trend-tracking only.
    """
    warnings = []
    for (book_type, mtype), row in sorted(results.items()):
        past = [
            int(h["p99_ns"]) for h in history
            if h["os"] == os_label and h["book_type"] == book_type
            and h["message_type"] == mtype
        ]
        past = past[-window:]
        if len(past) < min_history:
            continue
        median = statistics.median(past)
        if median <= 0:
            continue
        limit = median * multiplier
        if row["p99_ns"] > limit:
            warnings.append((book_type, mtype, row["p99_ns"], median, limit, len(past)))
    return warnings


def cmd_check(args):
    results = read_results(args.results)
    history = read_history(args.history)
    os_label = args.os or detect_os_label()

    print(f"== bench budget check (os={os_label}) ==")
    checked, failures = check_ratio(results, args.ratio_threshold)
    print(f"\n-- check 1: LadderBook must stay < {args.ratio_threshold:.0%} of "
          f"OrderBook p50 (hard fail) --")
    for mtype, order_p50, ladder_p50, ratio, limit in checked:
        status = "FAIL" if ladder_p50 >= limit else "ok"
        print(f"  [{status}] {mtype}: order_p50={order_p50}ns ladder_p50={ladder_p50}ns "
              f"ratio={ratio:.2%} (limit < {args.ratio_threshold:.0%})")
    if not checked:
        print("  no matching OrderBook/LadderBook rows found in results.csv")

    warnings = check_drift(
        results, history, os_label, args.history_window,
        args.drift_multiplier, args.min_history,
    )
    print(f"\n-- check 2: p99 drift vs last {args.history_window} CI runs on "
          f"{os_label} (informational) --")
    if not warnings:
        print("  no drift warnings")
    for book_type, mtype, current, median, limit, n in warnings:
        print(f"  WARNING: {book_type}/{mtype} p99={current}ns is {current / median:.2f}x its "
              f"{n}-run median ({median:.0f}ns), past warn threshold ({args.drift_multiplier:.1f}x)")

    if failures:
        print(f"\nFAIL: {len(failures)} message type(s) failed the LadderBook/OrderBook "
              f"ratio budget.")
        return 1

    suffix = f" ({len(warnings)} drift warning(s), non-blocking)" if warnings else ""
    print(f"\nPASS: ratio budget satisfied{suffix}")
    return 0


def cmd_record(args):
    results = read_results(args.results)
    history = read_history(args.history)
    os_label = args.os or detect_os_label()
    commit_sha = args.commit or detect_commit_sha()
    timestamp = args.timestamp or datetime.now(timezone.utc).isoformat(timespec="seconds")

    existing = {
        (h["commit_sha"], h["os"], h["book_type"], h["message_type"]) for h in history
    }

    new_rows = []
    for (book_type, mtype), row in sorted(results.items()):
        key = (commit_sha, os_label, book_type, mtype)
        if key in existing:
            continue
        new_rows.append({
            "timestamp_utc": timestamp,
            "commit_sha": commit_sha,
            "os": os_label,
            "book_type": book_type,
            "message_type": mtype,
            "p50_ns": row["p50_ns"],
            "p99_ns": row["p99_ns"],
            "p999_ns": row["p999_ns"],
        })

    if not new_rows:
        print(f"record: {commit_sha}/{os_label} already present in {args.history}, "
              f"nothing to do")
        return 0

    write_header = not os.path.exists(args.history)
    with open(args.history, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=HISTORY_FIELDS)
        if write_header:
            writer.writeheader()
        writer.writerows(new_rows)

    print(f"record: appended {len(new_rows)} row(s) for {commit_sha}/{os_label} "
          f"to {args.history}")
    return 0


def build_parser():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--results", default=DEFAULT_RESULTS)
    p.add_argument("--history", default=DEFAULT_HISTORY)
    p.add_argument("--os", default=None,
                    help="override the runner-os label (default: $RUNNER_OS or "
                         "platform.system())")
    sub = p.add_subparsers(dest="cmd", required=True)

    check_p = sub.add_parser("check", help="run the hard ratio gate + soft drift warning")
    check_p.add_argument("--ratio-threshold", type=float, default=0.90,
                          help="LadderBook p50 must be < this fraction of OrderBook p50 "
                               "(default 0.90)")
    check_p.add_argument("--drift-multiplier", type=float, default=1.75,
                          help="warn if p99 exceeds this multiple of its recent history "
                               "median (default 1.75)")
    check_p.add_argument("--history-window", type=int, default=10,
                          help="how many most recent history rows to compare against "
                               "(default 10)")
    check_p.add_argument("--min-history", type=int, default=3,
                          help="minimum history rows required before drift warnings apply "
                               "(default 3)")
    check_p.set_defaults(func=cmd_check)

    record_p = sub.add_parser("record",
                               help="append this run's results to the rolling CI history file")
    record_p.add_argument("--commit", default=None,
                           help="override commit sha (default: $GITHUB_SHA or git rev-parse)")
    record_p.add_argument("--timestamp", default=None,
                           help="override timestamp (default: now, UTC)")
    record_p.set_defaults(func=cmd_record)

    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

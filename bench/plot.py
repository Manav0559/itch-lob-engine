#!/usr/bin/env python3
"""Regenerate bench/plots/*.png from bench/results.csv.

Reads whatever run bench_main last produced -- no numbers are hand-picked or
computed here, only plotted. Requires matplotlib (pip install matplotlib).

Plots:
  per_msg_ns.png   batch-amortized per-message cost (p50 / p99 / p99.9 over
                   1024-message batches), "ALL" rows, log scale
  throughput.png   messages/second per book type, "ALL" rows
  per_type_p50.png per-call p50 by ITCH message type -- only written when the
                   CSV has per-type rows (x86-64 rdtsc runs)
"""
import csv
import pathlib
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib not found -- run: pip install matplotlib")

HERE = pathlib.Path(__file__).resolve().parent
CSV_PATH = HERE / "results.csv"
PLOTS_DIR = HERE / "plots"

PERCENTILES = [("p50_ns", "p50"), ("p99_ns", "p99"), ("p999_ns", "p99.9")]
MSG_TYPE_NAMES = {"A": "Add", "E": "Execute", "C": "Execute(price)", "X": "Cancel",
                  "D": "Delete", "U": "Replace"}
BOOK_ORDER = ["OrderBook", "LadderBook"]


def load_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def books_in(rows):
    present = {r["book_type"] for r in rows}
    return [b for b in BOOK_ORDER if b in present] + sorted(present - set(BOOK_ORDER))


def plot_per_msg(all_rows, out_path):
    books = books_in(all_rows)
    width = 0.8 / len(books)
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for i, book in enumerate(books):
        row = next(r for r in all_rows if r["book_type"] == book)
        vals = [float(row[k]) for k, _ in PERCENTILES]
        xs = [j + (i - (len(books) - 1) / 2) * width for j in range(len(PERCENTILES))]
        bars = ax.bar(xs, vals, width=width, label=book)
        ax.bar_label(bars, labels=[f"{v:.0f}" for v in vals], fontsize=8)
    ax.set_yscale("log")
    ax.set_xticks(range(len(PERCENTILES)))
    ax.set_xticklabels([label for _, label in PERCENTILES])
    ax.set_ylabel("ns per message (amortized over 1024-msg batches)")
    ax.set_title("OrderBook vs LadderBook -- per-message cost, batch-timed")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_throughput(all_rows, out_path):
    books = books_in(all_rows)
    vals = [float(next(r for r in all_rows if r["book_type"] == b)["msgs_per_sec"]) / 1e6
            for b in books]
    fig, ax = plt.subplots(figsize=(6, 4))
    bars = ax.bar(books, vals, color=["C0", "C1"][: len(books)])
    ax.bar_label(bars, labels=[f"{v:.2f}" for v in vals])
    ax.set_ylabel("million messages / second")
    ax.set_title("Book-update throughput (single thread)")
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def plot_per_type(type_rows, out_path):
    books = books_in(type_rows)
    types = [t for t in MSG_TYPE_NAMES if any(r["message_type"] == t for r in type_rows)]
    width = 0.8 / len(books)
    fig, ax = plt.subplots(figsize=(9, 4.5))
    for i, book in enumerate(books):
        vals = []
        for t in types:
            row = next((r for r in type_rows if r["book_type"] == book and r["message_type"] == t),
                       None)
            vals.append(float(row["p50_ns"]) if row else 0.0)
        xs = [j + (i - (len(books) - 1) / 2) * width for j in range(len(types))]
        ax.bar(xs, vals, width=width, label=book)
    ax.set_xticks(range(len(types)))
    ax.set_xticklabels([MSG_TYPE_NAMES[t] for t in types])
    ax.set_ylabel("ns per call (rdtsc, overhead subtracted)")
    ax.set_title("Per-type p50 (x86-64 rdtsc run)")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main():
    if not CSV_PATH.exists():
        sys.exit(f"{CSV_PATH} not found -- run ./build/bench first")
    rows = load_rows(CSV_PATH)
    if "msgs_per_sec" not in (rows[0] if rows else {}):
        sys.exit(f"{CSV_PATH} is in the old per-call format -- rerun ./build/bench")
    PLOTS_DIR.mkdir(exist_ok=True)

    all_rows = [r for r in rows if r["message_type"] == "ALL"]
    type_rows = [r for r in rows if r["message_type"] != "ALL"]
    outputs = []
    if all_rows:
        plot_per_msg(all_rows, PLOTS_DIR / "per_msg_ns.png")
        plot_throughput(all_rows, PLOTS_DIR / "throughput.png")
        outputs += ["per_msg_ns.png", "throughput.png"]
    if type_rows:
        plot_per_type(type_rows, PLOTS_DIR / "per_type_p50.png")
        outputs.append("per_type_p50.png")
    for name in outputs:
        print(f"wrote {PLOTS_DIR / name}")


if __name__ == "__main__":
    main()

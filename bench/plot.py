#!/usr/bin/env python3
"""Regenerate bench/plots/*.png from bench/results.csv.

Reads whatever run bench_main last produced — no numbers are hand-picked or
computed here, only plotted. Requires matplotlib (pip install matplotlib).
"""
import csv
import pathlib
import sys

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib not found — run: pip install matplotlib")

HERE = pathlib.Path(__file__).resolve().parent
CSV_PATH = HERE / "results.csv"
PLOTS_DIR = HERE / "plots"

PERCENTILES = ["p50_ns", "p99_ns", "p999_ns"]
MSG_TYPE_NAMES = {
    "A": "Add",
    "E": "Execute",
    "C": "Execute(price)",
    "X": "Cancel",
    "D": "Delete",
    "U": "Replace",
}


def load_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def plot_percentile(rows, percentile, out_path):
    msg_types = sorted({r["message_type"] for r in rows}, key=lambda t: list(MSG_TYPE_NAMES).index(t))
    book_types = sorted({r["book_type"] for r in rows})

    by_book = {
        book: [next(r for r in rows if r["book_type"] == book and r["message_type"] == t)[percentile]
               for t in msg_types]
        for book in book_types
    }

    x = range(len(msg_types))
    width = 0.8 / len(book_types)
    fig, ax = plt.subplots(figsize=(9, 5))
    for i, book in enumerate(book_types):
        values = [int(v) for v in by_book[book]]
        offsets = [xi + (i - (len(book_types) - 1) / 2) * width for xi in x]
        ax.bar(offsets, values, width=width, label=book)

    ax.set_xticks(list(x))
    ax.set_xticklabels([MSG_TYPE_NAMES.get(t, t) for t in msg_types])
    ax.set_ylabel("nanoseconds")
    ax.set_title(f"OrderBook vs LadderBook — {percentile}")
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main():
    if not CSV_PATH.exists():
        sys.exit(f"{CSV_PATH} not found — run bench_main first")
    rows = load_rows(CSV_PATH)
    PLOTS_DIR.mkdir(exist_ok=True)

    for percentile in PERCENTILES:
        out_path = PLOTS_DIR / f"{percentile}.png"
        plot_percentile(rows, percentile, out_path)
        print(f"wrote {out_path}")


if __name__ == "__main__":
    main()

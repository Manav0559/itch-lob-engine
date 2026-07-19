#!/usr/bin/env bash
# Wraps ./build/bench with whatever hardware-counter tool the current
# platform actually gives userspace access to. That's not the same tool on
# both CI platforms, and it never will be:
#
#   - Linux exposes cache/branch/cycle counters to userspace via
#     perf_event_open, which `perf stat` wraps. Full counter set.
#   - macOS (Apple Silicon and Intel alike) does not hand out PMU access to
#     unprivileged processes at all -- Apple gates it behind entitlements
#     ordinary `cmake --build` binaries don't have. There is no one-liner
#     equivalent to `perf stat` here, and this script does not pretend
#     otherwise: on macOS it falls back to `/usr/bin/time -l`, which reports
#     memory/fault/scheduling counters instead of cache/branch counters.
#     That's a smaller, honest subset -- see bench/HARDWARE_PROFILING.md for
#     what each platform's numbers mean and why the gap exists.
#
# Any argument given to this script is forwarded to ./build/bench unchanged
# (e.g. a path to a real .NASDAQ_ITCH50 or .NASDAQ_ITCH50.gz file).
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

BENCH_BIN="./build/bench"
if [[ ! -x "$BENCH_BIN" ]]; then
    echo "error: $BENCH_BIN not found or not executable." >&2
    echo "build it first: cmake --build build --target bench" >&2
    exit 1
fi

PLATFORM="$(uname -s)"

case "$PLATFORM" in
    Linux)
        if ! command -v perf >/dev/null 2>&1; then
            echo "error: perf not found on PATH." >&2
            echo "install it (e.g. 'apt-get install linux-tools-generic' on Ubuntu) or run under a runner that has it." >&2
            exit 1
        fi
        echo "hw_profile: Linux detected -- running under 'perf stat' (cache/branch/cycle counters via perf_event_open)"
        echo
        exec perf stat \
            -e cache-misses,cache-references,branch-misses,branch-instructions,cycles,instructions \
            -- "$BENCH_BIN" "$@"
        ;;
    Darwin)
        echo "hw_profile: macOS detected -- running under '/usr/bin/time -l'"
        echo "  (macOS does not grant userspace PMU access; cache-miss/branch-miss counters are"
        echo "   NOT available here without a kernel extension or special entitlements this repo"
        echo "   does not require. This reports memory/fault/scheduling counters instead -- a"
        echo "   deliberately smaller, honest subset. See bench/HARDWARE_PROFILING.md.)"
        echo
        exec /usr/bin/time -l "$BENCH_BIN" "$@"
        ;;
    *)
        echo "error: unsupported platform '$PLATFORM' -- this script only handles Linux and Darwin." >&2
        exit 1
        ;;
esac

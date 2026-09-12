#!/usr/bin/env bash
# Compiles fuzz_differential.cpp with clang's libFuzzer + ASan + UBSan and
# smoke-runs it against fuzz/corpus/ for a bounded time. Mirrors
# build_and_run.sh's structure and reasoning exactly (same "standalone
# script, not wired into CMakeLists.txt/ctest" call, same macOS
# Homebrew-LLVM-clang fallback) — see that script's own header comment for
# why. The only real differences: a different source file/binary name, and
# this harness's own findings subdirectory so a differential-fuzzer session
# never mixes its coverage-increasing corpus growth into fuzz_parser's.
#
# Usage:
#   fuzz/build_and_run_differential.sh [seconds]
#   FUZZ_SECONDS=300 fuzz/build_and_run_differential.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOMEBREW_LLVM_CLANG="/opt/homebrew/opt/llvm/bin/clang++"

if [ -x "${HOMEBREW_LLVM_CLANG}" ]; then
    CLANGXX="${HOMEBREW_LLVM_CLANG}"
elif command -v clang++ >/dev/null 2>&1; then
    CLANGXX="$(command -v clang++)"
else
    echo "error: clang++ not found." >&2
    echo "       libFuzzer requires clang (not gcc/g++). On macOS, the Xcode" >&2
    echo "       Command Line Tools clang lacks the libFuzzer runtime — run" >&2
    echo "       'brew install llvm' and re-run this script. On Linux, install" >&2
    echo "       clang from your package manager." >&2
    exit 1
fi

DURATION="${1:-${FUZZ_SECONDS:-60}}"
BIN="${SCRIPT_DIR}/fuzz_differential"
CORPUS_DIR="${SCRIPT_DIR}/corpus"
REGRESSIONS_DIR="${CORPUS_DIR}/regressions"
# Same corpus/regressions seeds as fuzz_parser (both harnesses decode the
# same wire format), but its own findings subdirectory: libFuzzer writes
# every coverage-increasing input it discovers back into the first directory
# it's given, and this harness explores different coverage (both books, not
# just decode) than fuzz_parser does, so the two shouldn't share one
# findings pool.
FINDINGS_DIR="${SCRIPT_DIR}/findings/differential"

mkdir -p "${CORPUS_DIR}" "${REGRESSIONS_DIR}" "${FINDINGS_DIR}"

echo "building ${BIN} with ${CLANGXX}..."
"${CLANGXX}" -std=c++20 -I"${REPO_ROOT}/include" \
    -fsanitize=fuzzer,address,undefined -g -O1 \
    -o "${BIN}" "${SCRIPT_DIR}/fuzz_differential.cpp"

echo "running for ${DURATION}s (seeded from ${CORPUS_DIR} and its regressions/)..."
"${BIN}" \
    -max_total_time="${DURATION}" \
    -artifact_prefix="${SCRIPT_DIR}/" \
    "${FINDINGS_DIR}" "${CORPUS_DIR}" "${REGRESSIONS_DIR}"

echo "clean: no book divergence found in ${DURATION}s"

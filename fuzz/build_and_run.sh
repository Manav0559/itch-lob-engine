#!/usr/bin/env bash
# Compiles fuzz_parser.cpp with clang's libFuzzer + ASan + UBSan and smoke-runs
# it against fuzz/corpus/ for a bounded time. Deliberately not wired into
# CMakeLists.txt: libFuzzer isn't available across this project's Linux/macOS
# CI matrix the same uniform way the normal build is, and mixing sanitizer
# flags into the main -Werror build would be its own mess. A standalone
# script is the right amount of integration for a fuzz smoke test.
#
# Usage:
#   fuzz/build_and_run.sh [seconds]
#   FUZZ_SECONDS=300 fuzz/build_and_run.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOMEBREW_LLVM_CLANG="/opt/homebrew/opt/llvm/bin/clang++"

# On macOS, the Xcode Command Line Tools' clang++ ships ASan/UBSan but not
# libclang_rt.fuzzer_osx.a, so -fsanitize=fuzzer fails at link time. Prefer a
# Homebrew LLVM clang++ when one is present; only fall back to whatever's on
# PATH otherwise (e.g. Linux, where the system clang++ already bundles it).
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
BIN="${SCRIPT_DIR}/fuzz_parser"
CORPUS_DIR="${SCRIPT_DIR}/corpus"
REGRESSIONS_DIR="${CORPUS_DIR}/regressions"
# libFuzzer writes every new coverage-increasing input it discovers back into
# the first corpus directory it's given. Pointing that at FINDINGS_DIR
# (gitignored) instead of CORPUS_DIR keeps the hand-picked seeds in
# fuzz/corpus/ stable across runs — a 60s run finds hundreds of mutations,
# and committing those as if they were curated seeds would be noise, not
# signal. CORPUS_DIR and REGRESSIONS_DIR are still read as additional seed
# input on every run, just never written to.
FINDINGS_DIR="${SCRIPT_DIR}/findings"

mkdir -p "${CORPUS_DIR}" "${REGRESSIONS_DIR}" "${FINDINGS_DIR}"

echo "building ${BIN} with ${CLANGXX}..."
"${CLANGXX}" -std=c++20 -I"${REPO_ROOT}/include" \
    -fsanitize=fuzzer,address,undefined -g -O1 \
    -o "${BIN}" "${SCRIPT_DIR}/fuzz_parser.cpp"

echo "running for ${DURATION}s (seeded from ${CORPUS_DIR} and its regressions/)..."
"${BIN}" \
    -max_total_time="${DURATION}" \
    -artifact_prefix="${SCRIPT_DIR}/" \
    "${FINDINGS_DIR}" "${CORPUS_DIR}" "${REGRESSIONS_DIR}"

echo "clean: no crash found in ${DURATION}s"

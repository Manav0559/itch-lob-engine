#!/usr/bin/env bash
# Fetches a real NASDAQ ITCH 5.0 day file from NASDAQ's public sample archive
# (https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/) using many parallel byte-range
# curl requests instead of one plain `curl -o file url`.
#
# Why this exists: a single connection to emi.nasdaq.com was observed capped
# at well under 100 KB/s from this project's dev environment -- at that rate
# a multi-GB day file takes the better part of a day, and a session that
# ends (or a flaky connection that drops) before it finishes leaves a
# truncated .gz sitting in the repo root that `gzip -t` rejects with
# "unexpected end of file". That's exactly what happened once before this
# script existed -- see docs/devlog-orderbook-vs-ladderbook.md. Splitting the
# same download into many concurrent range requests (each apparently gets
# its own throttle bucket) turned an infeasible multi-hour single-stream
# download into a double-digit-minutes one in testing.
#
# Usage:
#   bench/fetch_itch_day.sh <name> [output_dir] [parallelism] [chunk_mb]
#
#   <name>        NASDAQ's date-coded file stem, e.g. 12302019 for
#                 12302019.NASDAQ_ITCH50.gz (see the directory listing at
#                 the URL above for what's currently published).
#   output_dir    Where to write <name>.NASDAQ_ITCH50.gz (default: .)
#   parallelism   Concurrent range requests per batch (default: 16)
#   chunk_mb      Size of each range request in MiB (default: 32)
#
# Verifies the assembled file's size against the server's Content-Length and
# runs `gzip -t` on it before reporting success -- a partial or corrupt
# result is a non-zero exit, never a silent truncated file left behind.
set -euo pipefail

NAME="${1:?usage: fetch_itch_day.sh <name e.g. 12302019> [output_dir] [parallelism] [chunk_mb]}"
OUT_DIR="${2:-.}"
PARALLEL="${3:-16}"
CHUNK_MB="${4:-32}"

URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${NAME}.NASDAQ_ITCH50.gz"
OUT="${OUT_DIR}/${NAME}.NASDAQ_ITCH50.gz"
WORK=$(mktemp -d "${OUT_DIR}/.fetch_itch_${NAME}.XXXXXX")
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo "Fetching Content-Length for $URL ..."
TOTAL=$(curl -sI -A "Mozilla/5.0" "$URL" | tr -d '\r' | awk 'tolower($1)=="content-length:"{print $2}')
if [[ -z "${TOTAL:-}" ]]; then
  echo "Could not determine remote file size (HEAD request failed) -- aborting." >&2
  exit 1
fi
echo "Remote size: ${TOTAL} bytes"

CHUNK_BYTES=$((CHUNK_MB * 1024 * 1024))
N_CHUNKS=$(( (TOTAL + CHUNK_BYTES - 1) / CHUNK_BYTES ))
echo "Downloading ${N_CHUNKS} chunks of ${CHUNK_MB}MB each, ${PARALLEL} at a time..."

fetch_chunk() {
  local idx="$1" start="$2" end="$3"
  local part="$WORK/part_$(printf '%06d' "$idx")"
  local expect=$((end - start + 1))
  local tries=0 got
  while true; do
    tries=$((tries + 1))
    if curl -sf -A "Mozilla/5.0" -r "${start}-${end}" -o "$part" "$URL"; then
      got=$(wc -c < "$part" | tr -d ' ')
      if [[ "$got" -eq "$expect" ]]; then
        return 0
      fi
    fi
    if [[ "$tries" -ge 5 ]]; then
      echo "chunk ${idx} (bytes ${start}-${end}) failed after ${tries} attempts" >&2
      return 1
    fi
    sleep $((tries * 2))
  done
}

i=0
while [[ "$i" -lt "$N_CHUNKS" ]]; do
  batch_end=$((i + PARALLEL))
  if [[ "$batch_end" -gt "$N_CHUNKS" ]]; then batch_end=$N_CHUNKS; fi
  j=$i
  while [[ "$j" -lt "$batch_end" ]]; do
    start=$((j * CHUNK_BYTES))
    end=$((start + CHUNK_BYTES - 1))
    if [[ "$end" -ge "$TOTAL" ]]; then end=$((TOTAL - 1)); fi
    fetch_chunk "$j" "$start" "$end" &
    j=$((j + 1))
  done
  wait
  echo "  chunks ${i}..$((batch_end - 1)) of ${N_CHUNKS} done"
  i=$batch_end
done

echo "Assembling ${OUT} ..."
: > "$OUT"
i=0
while [[ "$i" -lt "$N_CHUNKS" ]]; do
  cat "$WORK/part_$(printf '%06d' "$i")" >> "$OUT"
  i=$((i + 1))
done

ACTUAL=$(wc -c < "$OUT" | tr -d ' ')
if [[ "$ACTUAL" -ne "$TOTAL" ]]; then
  echo "Size mismatch after assembly: got ${ACTUAL}, expected ${TOTAL}" >&2
  exit 1
fi

echo "Verifying gzip integrity..."
gzip -t "$OUT"
echo "OK: ${OUT} (${ACTUAL} bytes, gzip -t passed)"

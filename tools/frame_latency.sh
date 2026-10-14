#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Frame latency on the T6 while a command runs (mct_t6 0.29.11 and later):
# from the first commit of a frame until the worker starts, and until the
# frame's flip transfer has completed on the device; plus the conversion and
# USB time per update, the commit time, and frames replaced before they
# went out. Reads debugfs (root).
#
#   sudo tools/frame_latency.sh SECONDS [label]          measure while you use it
#   sudo tools/frame_latency.sh SECONDS label -- CMD...  run CMD meanwhile (killed after)
set -eu
secs=${1:?seconds}; shift
label=${1:-run}; [ $# -gt 0 ] && shift
[ "${1:-}" = -- ] && shift
f=$(ls /sys/kernel/debug/dri/*/mct_t6_stats 2>/dev/null | head -1)
[ -n "$f" ] || { echo "no mct_t6_stats in debugfs (driver loaded? root?)" >&2; exit 1; }
grep -q "^frame to device" "$f" || { echo "the loaded mct_t6 has no frame latency statistics (needs 0.29.11)" >&2; exit 1; }

# key avg count, from the lines that carry an average and its count
snap() {
	awk '/^frame to worker/ { print "worker", $4, $9 }
	     /^frame to device/ { print "device", $4, $9 }
	     /^updates / { print "updates", 0, $2 }
	     /^skipped images/ { print "skipped", 0, $3 }
	     /^ms per update/ { c = $5; sub(/,/, "", c); print "convert", c, 0; print "usb", $7, 0 }' "$f"
}

a=$(mktemp); b=$(mktemp); trap 'rm -f "$a" "$b"' EXIT
snap > "$a"
if [ $# -gt 0 ]; then
	"$@" >/dev/null 2>&1 & pid=$!
	sleep "$secs"
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
else
	sleep "$secs"
fi
snap > "$b"
awk -v label="$label" -v secs="$secs" '
	NR == FNR { a[$1] = $2; n[$1] = $3; next }
	{ b[$1] = $2; m[$1] = $3 }
	END {
		u = m["updates"] - n["updates"]
		printf "%s: %d updates in %d s (%.1f per second), %d images replaced before they went out\n",
			label, u, secs, u / secs, m["skipped"] - n["skipped"]
		for (k in b) if (k == "worker" || k == "device") {
			c = m[k] - n[k]
			if (c > 0)
				printf "  commit -> %-6s %6.2f ms average over %d frames\n", k,
					(b[k] * m[k] - a[k] * n[k]) / c, c
		}
		printf "  (since load: conversion %s ms, USB queueing %s ms per update)\n", b["convert"], b["usb"]
	}' "$a" "$b"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Cursor latency per phase, from the driver's debugfs "cursor" lines (since
# mct 0.6.x): time from the compositor's commit to the end of the USB
# transfer, for moves and for new cursor images. Runs as root (debugfs).
#
#   sudo tools/cursor_latency.sh [t6|t2] [seconds per phase, default 10]
#
# Each phase: press Enter, do what it says for the given time, read the
# numbers. Nothing is sent to the device; it only reads statistics.
set -eu
chip=${1:-t6}
secs=${2:-10}
f=$(ls /sys/kernel/debug/dri/*/mct_${chip}_stats 2>/dev/null | head -1)
[ -n "$f" ] || { echo "no mct_${chip}_stats in debugfs (driver loaded? root?)" >&2; exit 1; }
grep -q "^ms per cursor move" "$f" || {
	echo "the loaded driver has no cursor statistics (needs mct_t6 0.29.9 / mct_t2 0.3.37;" >&2
	echo "loaded: $(cat /sys/bus/usb/devices/*/mct/driver_version 2>/dev/null | tr '\n' ' '))" >&2
	exit 1
}

# "ms per cursor move  A (max B) over N moves" -> "A N"
snap() {
	awk '/^ms per cursor move/ { print "move", $5, $9 }
	     /^ms per cursor shape/ { print "shape", $5, $9 }
	     /^cursor / { print "commits", $2, $4 }' "$f"
}

delta() {	# before after -> per-phase average and counts
	awk -v name="$1" '
		NR == FNR { a[$1] = $2; n[$1] = $3; next }
		{ b[$1] = $2; m[$1] = $3 }
		END {
			c = b["commits"] - a["commits"]; r = m["commits"] - n["commits"]
			printf "  %s: %d cursor commits, %d replaced before sending\n", name, c, r
			for (k in b) if (k != "commits") {
				cnt = m[k] - n[k]
				if (cnt > 0)
					printf "    %-5s %5d, average %.2f ms commit -> device\n", k, cnt,
						(b[k] * m[k] - a[k] * n[k]) / cnt
			}
		}' "$2" "$3"
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "reading $f"
phases=(
	"move the mouse in circles over a still desktop on this display"
	"play a fullscreen video on this display and move the mouse in circles over it"
	"drag a large window around on this display, fast"
	"hover text fields and links so the cursor changes shape often"
)
i=0
for p in "${phases[@]}"; do
	i=$((i + 1))
	read -r -p "phase $i: $p for $secs s. Press Enter to start... "
	snap > "$tmp/a"
	sleep "$secs"
	snap > "$tmp/b"
	echo "  done."
	delta "phase $i" "$tmp/a" "$tmp/b"
done
echo
echo "max values since the driver loaded:"
grep "^ms per cursor" "$f"

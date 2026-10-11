#!/usr/bin/env bash
# Which modes would mct_t6 offer other monitors? Feeds each profile of
# edid_gen.py to the T6's connector through DRM's edid_override, re-probes,
# saves modetest's mode list to ../../captures/t6-edid-sim/, then restores
# the real EDID. Turn the T6 output off in niri first, so nothing gets set:
#   niri msg output HDMI-A-3 off ; tools/t6_edid_sim.sh ; niri msg output HDMI-A-3 on
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if [ "$(id -u)" != 0 ]; then
	mt=$(nix build --no-link --print-out-paths nixpkgs#libdrm.bin)/bin/modetest
	exec sudo MT="$mt" PY="$(command -v python3)" SUDO_UID="$(id -u)" "$0" "$@"
fi
out="$here/../../captures/t6-edid-sim"
mkdir -p "$out"

dbg=$(dirname "$(ls /sys/kernel/debug/dri/*/mct_t6_stats | head -1)")
conn=$(basename "$(ls -d "$dbg"/HDMI-A-* | head -1)")
status=$(ls /sys/class/drm/card*-"$conn"/status | head -1)
echo "debugfs $dbg, connector $conn"

probe() {
	echo detect > "$status"
	sleep 1
	"$MT" -M mct_t6 -c > "$out/raw-$1.txt" 2>&1 || true
	sed -n '/modes:/,/props:/p' "$out/raw-$1.txt" | grep -E "^\s+#" || true
}

profiles=${*:-$("$PY" "$here/edid_gen.py" | awk '{print $1}')}
for p in $profiles; do
	"$PY" "$here/edid_gen.py" "$p" > "$dbg/$conn/edid_override"
	{ "$PY" "$here/edid_gen.py" | grep "^$p "; probe "$p"; } > "$out/$p.txt"
	echo "== $p: $(grep -c '#' "$out/$p.txt") modes"
	grep -E "^\s+#[0-3] " "$out/$p.txt" | awk '{print "   ", $2, $3, "Hz", $12 / 1000, "MHz", (/preferred/ ? "preferred" : "")}' || true
done
printf reset > "$dbg/$conn/edid_override"	# exactly "reset", no newline
probe real > "$out/real.txt"
echo "== real EDID back: $(grep -c '#' "$out/real.txt") modes"
chown -R "$SUDO_UID" "$out"

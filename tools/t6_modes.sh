#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Switch a niri output through modes one at a time, waiting for y/n after
# each (some monitors take seconds to show a picture); summary at the end.
#   tools/t6_modes.sh HDMI-A-3 2560x1440@59.951 3440x1440@55.000 3440x1440@60.000
out=$1
shift
res=()
for m in "$@"; do
	niri msg output "$out" mode "$m" || { res+=("$m: niri refused"); continue; }
	while :; do
		read -r -p "$m: picture ok? y/n + Enter: " a
		[ "$a" = y ] || [ "$a" = n ] && break
	done
	res+=("$m: $([ "$a" = y ] && echo ok || echo BAD)")
done
printf '\nsummary:\n'
printf '  %s\n' "${res[@]}"

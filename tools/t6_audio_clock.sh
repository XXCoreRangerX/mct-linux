#!/usr/bin/env bash
# Find what makes T6 HDMI audio buzz (2026-10-12: clean at 1080p120, buzzing
# at the driver's 1080p144). Each step sets a mode through niri, waits for
# the picture, plays a tone and asks. Restores the starting mode on exit.
# Result: horizontal blanking, not the clock (see below and
# docs/history/t6-lab-notebook.md).
#
#   tools/t6_audio_clock.sh [clock|hblank] [OUTPUT [CARD]]
#                                       default clock HDMI-A-3 JUA254H
#
# clock: bisects the pixel clock with the CTA 1080p timing (2200 x 1125)
# and only the clock changed (refresh = clock / 2.475 MHz). Result (2026-10-12): 333.216 MHz with the CTA timing was
# clean, so the clock is not the limit; the driver's 144 Hz mode has only
# 80 px horizontal blanking (its tight fallback timing), and HDMI carries
# audio in the blanking.
# hblank: 1080p at 120 Hz (vtotal 1125, sync 8 px after the active area,
# 32 px wide, as the driver's tight timing), the horizontal blanking
# bisected between 80 and 280 px (CTA) in 8 px steps; clocks 270-297 MHz.
# Result (2026-10-12): 104 px buzzing, 112 px clean.
#
# Check the monitor's range first (EDID range descriptor): the LG 27GN7
# takes 48-240 Hz and up to 255 kHz; this script stays at 120-135 Hz,
# 135-151 kHz.
set -u
what=clock
case ${1:-} in clock|hblank) what=$1; shift ;; esac
out=${1:-HDMI-A-3}
card=${2:-JUA254H}

start=$(niri msg outputs | awk -v o="($out)" '
	index($0, o) { f = 1 } f && /Current mode:/ { print $3 "@" $5; exit }')
[ -n "$start" ] || { echo "output $out not found"; exit 1; }
echo "starting mode: $start (restored at the end)"
restore() { echo "restoring $start"; niri msg output "$out" mode "$start"; }
trap restore EXIT
trap 'exit 130' INT

ask() {	# ask "question" -> 0 for y, 1 for n
	local a
	while read -r -p "  $1 y/n + Enter: " a; do
		case $a in y) return 0 ;; n) return 1 ;; esac
	done
	exit 1
}

# set_mode WHAT VALUE: clock in kHz (CTA timing) or hblank in px (120 Hz)
set_mode() {
	local mhz hz
	if [ "$1" = clock ]; then
		mhz=$(awk -v k="$2" 'BEGIN { printf "%.3f", k / 1000 }')
		hz=$(awk -v k="$2" 'BEGIN { printf "%.2f", k * 1000 / 2475000 }')
		echo "== $mhz MHz, 1920x1080 @ $hz Hz, CTA timing"
		label="$mhz MHz"
		niri msg output "$out" modeline "$mhz" 1920 2008 2052 2200 1080 1084 1089 1125 +hsync +vsync
	else
		mhz=$(awk -v b="$2" 'BEGIN { printf "%.3f", (1920 + b) * 1125 * 120 / 1e6 }')
		echo "== horizontal blanking $2 px (htotal $((1920 + $2))), 120 Hz, $mhz MHz"
		label="hblank $2 px"
		niri msg output "$out" modeline "$mhz" 1920 1928 1960 $((1920 + $2)) 1080 1084 1089 1125 +hsync -vsync
	fi
}

# try VALUE -> 0 clean, 1 buzzing
try() {
	echo
	set_mode "$what" "$1"
	if ! ask "picture back on the monitor?"; then
		echo "  no picture: stopping here"
		exit 1
	fi
	echo "  driver: $(cat /sys/bus/usb/devices/*/mct/mode 2>/dev/null | head -1)"
	speaker-test -D "hw:$card,0" -c 2 -r 48000 -F S16_LE -t sine -l 1 >/dev/null 2>&1
	if ask "tone clean (left, then right)?"; then
		echo "RESULT $label clean" >> "$log"
		return 0
	fi
	echo "RESULT $label buzzing" >> "$log"
	return 1
}

log=$(mktemp --tmpdir t6_audio_clock.XXXXXX.log)
echo "log: $log"

if [ "$what" = clock ]; then
	lo=297000	# kHz, known clean
	hi=333216	# kHz, buzzing at the driver's 144 Hz timing
	# the upper end with this timing first: is it the clock or the timing?
	if try "$hi"; then
		echo "$hi kHz is clean with the CTA timing: the timing, not the clock, matters" |
			tee -a "$log"
		exit 0
	fi
	while [ $((hi - lo)) -gt 1000 ]; do
		mid=$(((lo + hi) / 2))
		if try "$mid"; then lo=$mid; else hi=$mid; fi
	done
	echo
	echo "clean up to $lo kHz, buzzing from $hi kHz (CTA 1080p timing)" | tee -a "$log"
	exit 0
fi

hi=280	# px, CTA: clean
lo=80	# px, the driver's tight timing: buzzing at 144 Hz
# the lower end at 120 Hz first: is it the blanking?
if try "$lo"; then
	echo "80 px is clean at 120 Hz: not the blanking alone" | tee -a "$log"
	exit 0
fi
while [ $((hi - lo)) -gt 8 ]; do
	mid=$(((lo + hi) / 2 / 8 * 8))
	if try "$mid"; then hi=$mid; else lo=$mid; fi
done
echo
echo "buzzing at $lo px horizontal blanking, clean from $hi px (1080p 120 Hz)" | tee -a "$log"

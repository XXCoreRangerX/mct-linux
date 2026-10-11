#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
#
# Interactive edge-case checks for the mct driver that only real hardware
# can answer: cursor at the screen edges, format and lossy changes under
# motion, all_modes, text consoles and VT switches, USB unplug with sysfs
# readers running, DPMS, monitor power, suspend. Each step says what to do
# and what to look for and waits for y/n (s = skip, q = quit). Kernel log
# lines matching mct|BUG|Oops|WARN|NULL are shown after each step, a summary
# at the end, and everything goes to a log file as well.
#
# Safe steps come first; risky ones (a monitor may lose its signal, or a
# replug is needed) say so in one line and can be skipped.
#
# Usage:  tools/mct_edge_test.sh [first step]
#         MCT_T6=2-2:1.0 tools/mct_edge_test.sh   (pick one of several T6s)
# Needs:  bash, coreutils, util-linux (dmesg), sudo, niri msg. The console
#         steps also need con2fbmap (package fbset; on NixOS run the script
#         inside "nix shell nixpkgs#fbset"), or CON2FBMAP=/path/to/con2fbmap.
set -u

START=${1:-1}
LOG="${TMPDIR:-/tmp}/mct_edge_test.$(date +%Y%m%d-%H%M%S).log"
LOGRE='mct|BUG|Oops|WARN|NULL|Call Trace|blocked for more than|soft lockup'
BADRE='BUG|Oops|WARNING|NULL pointer|Call Trace|blocked for more than|soft lockup'
C2F=${CON2FBMAP:-$(command -v con2fbmap 2>/dev/null)}
NIRI_VT=1
if read -r _vt < /sys/class/tty/tty0/active 2>/dev/null; then NIRI_VT=${_vt#tty}; fi
TTY6=3; [ "$NIRI_VT" = 3 ] && TTY6=5
TTY2=4; [ "$NIRI_VT" = 4 ] && TTY2=6

logf() { printf '%s\n' "$*" >> "$LOG"; }
say() { printf '%s\n' "$*"; logf "$*"; }
pause() { read -r -p "$1 [Enter] " _; logf "> $1"; }
rd() { local v; if read -r v 2>/dev/null < "$1"; then printf '%s' "$v"; else printf '?'; fi; }

# write a sysfs value: directly if the udev rule made it writable, else sudo
put() {
	if ! { printf '%s\n' "$2" > "$1"; } 2>/dev/null; then
		printf '%s\n' "$2" | sudo tee "$1" > /dev/null
	fi
	logf "  wrote $2 > $1"
}

# ------------------------------------------------------------------ devices

# T6, T2: .../mct directories of the bound devices (first of each chip)
find_devs() {
	local d chip
	T6= T2=
	for d in /sys/bus/usb/drivers/mct/*/mct; do
		[ -r "$d/chip" ] || continue
		chip=$(rd "$d/chip")
		if [ "$chip" = t6 ] && [ -z "$T6" ]; then
			if [ -z "${MCT_T6:-}" ] || [ "$d" = "/sys/bus/usb/drivers/mct/$MCT_T6/mct" ]; then
				T6=$d
			fi
		elif [ "$chip" = t2 ] && [ -z "$T2" ]; then
			T2=$d
		fi
	done
}

intf_of() { local d=${1%/mct}; printf '%s' "${d##*/}"; }
conn_of() { local c; c=$(rd "$1/connector"); printf '%s' "${c#card*-}"; }

# fbdev index of an interface, or -
fb_of() {
	local want f
	want=$(readlink -f "/sys/bus/usb/devices/$1")
	for f in /sys/class/graphics/fb[0-9]*; do
		if [ "$(readlink -f "$f/device")" = "$want" ]; then
			printf '%s' "${f##*/fb}"
			return
		fi
	done
	printf -- -
}

# debugfs directory of an interface (root only), or empty
dbg_of() {
	local d name
	for d in $(sudo ls /sys/kernel/debug/dri 2>/dev/null); do
		name=$(sudo cat "/sys/kernel/debug/dri/$d/name" 2>/dev/null) || continue
		if [[ $name == *"$1"* ]]; then
			printf '%s' "/sys/kernel/debug/dri/$d"
			return
		fi
	done
}

# the mode, events and recoveries lines of the T6's debugfs statistics
t6_stats() {
	local dbg line
	dbg=$(dbg_of "$(intf_of "$T6")")
	[ -n "$dbg" ] || { say "  (no debugfs statistics found)"; return; }
	while IFS= read -r line; do
		[[ $line =~ ^(mode|events|recoveries) ]] && say "  $line"
	done < <(sudo cat "$dbg/mct_t6_stats" 2>/dev/null)
}

show_dev() {
	local f
	say "  $(intf_of "$1"):"
	for f in "$1"/*; do
		say "$(printf '    %-14s %s' "${f##*/}" "$(rd "$f")")"
	done
	say "$(printf '    %-14s %s' fbdev "fb$(fb_of "$(intf_of "$1")")")"
}

# "1920x1080 @ 59.973" or "1920x1080@59.973" -> 1920x1080@60 (as the driver rounds)
norm_mode() {
	local s=${1// /} wh hz int frac
	wh=${s%%@*}
	hz=${s#*@}
	int=${hz%%.*}
	frac=${hz#*.}
	[ "$frac" = "$hz" ] && frac=0
	((10#${frac:0:1} >= 5)) && int=$((int + 1))
	printf '%s@%s' "$wh" "$int"
}

# niri's view of a connector: NIRI_CUR (or off), NIRI_PREF, NIRI_NMODES
niri_info() {
	local line in=0
	NIRI_CUR=off NIRI_PREF=- NIRI_NMODES=0
	while IFS= read -r line; do
		if [[ $line == Output* ]]; then
			in=0
			[[ $line == *"($1)" ]] && in=1
			continue
		fi
		((in)) || continue
		if [[ $line =~ Current\ mode:\ ([0-9]+x[0-9]+)\ @\ ([0-9.]+) ]]; then
			NIRI_CUR=$(norm_mode "${BASH_REMATCH[1]}@${BASH_REMATCH[2]}")
		elif [[ $line =~ ^[[:space:]]+([0-9]+x[0-9]+)@([0-9.]+) ]]; then
			NIRI_NMODES=$((NIRI_NMODES + 1))
			[[ $line == *preferred* ]] &&
				NIRI_PREF=$(norm_mode "${BASH_REMATCH[1]}@${BASH_REMATCH[2]}")
		fi
	done < <(niri msg outputs 2>/dev/null)
}

# driver mode vs. niri's for a device directory; sets MODE_OK
check_mode() {
	local drv conn
	drv=$(rd "$1/mode")
	conn=$(conn_of "$1")
	niri_info "$conn"
	MODE_OK=0
	[ "$drv" = "$NIRI_CUR" ] && MODE_OK=1
	say "  $conn: driver mode $drv, niri $NIRI_CUR (preferred $NIRI_PREF, $NIRI_NMODES modes)$( ((MODE_OK)) || echo '  <- differ')"
}

# ------------------------------------------------------------------ kernel log

MARK=0
log_mark() {
	local l
	l=$(sudo dmesg --color=never 2>/dev/null | tail -n 1)
	MARK=0
	[[ $l =~ ^\[\ *([0-9]+)\.([0-9]{6})\] ]] && MARK=$((10#${BASH_REMATCH[1]}${BASH_REMATCH[2]}))
}

# kernel lines since MARK matching LOGRE: printed, counted in LOGN / LOGBAD
log_since() {
	local l ts
	LOGN=0 LOGBAD=0
	while IFS= read -r l; do
		[[ $l =~ ^\[\ *([0-9]+)\.([0-9]{6})\] ]] || continue
		ts=$((10#${BASH_REMATCH[1]}${BASH_REMATCH[2]}))
		((ts > MARK)) || continue
		[[ $l =~ $LOGRE ]] || continue
		((LOGN == 0)) && say "  kernel log:"
		LOGN=$((LOGN + 1))
		if [[ $l =~ $BADRE ]]; then
			LOGBAD=$((LOGBAD + 1))
			say "  !! $l"
		else
			say "     $l"
		fi
	done < <(sudo dmesg --color=never 2>/dev/null)
	((LOGN)) || say "  kernel log: nothing matching"
}

# ------------------------------------------------------------------ sysfs reader

# read every file of dir in a loop until killed; counts go to out after each pass
sysfs_reader() {
	local dir=$1 out=$2 f v t0 t1 ms ok=0 err=0 slow=0 max=0
	while :; do
		for f in "$dir"/*; do
			t0=${EPOCHREALTIME//[!0-9]/}
			if read -r v 2>/dev/null < "$f"; then ok=$((ok + 1)); else err=$((err + 1)); fi
			t1=${EPOCHREALTIME//[!0-9]/}
			ms=$(((t1 - t0) / 1000))
			((ms > max)) && max=$ms
			((ms > 500)) && slow=$((slow + 1))
		done
		printf '%d %d %d %d\n' "$ok" "$err" "$slow" "$max" > "$out"
		sleep 0.05
	done
}

READER= READER_OUT=
reader_start() {
	READER_OUT=$(mktemp)
	sysfs_reader "$1" "$READER_OUT" &
	READER=$!
	say "  reading $1/* in a loop (pid $READER)"
}

reader_stop() {
	local i ok=0 err=0 slow=0 max=0
	[ -n "$READER" ] || return
	kill "$READER" 2>/dev/null
	for i in $(seq 30); do
		kill -0 "$READER" 2>/dev/null || break
		sleep 0.1
	done
	if kill -0 "$READER" 2>/dev/null; then
		say "  !! the sysfs reader did not stop: a read is stuck in the kernel (pid $READER)"
		kill -9 "$READER" 2>/dev/null
	else
		wait "$READER" 2>/dev/null
	fi
	read -r ok err slow max < "$READER_OUT" 2>/dev/null
	say "  sysfs reads: $ok fine, $err failed (expected while unplugged), $slow over 0.5 s, longest $max ms"
	rm -f "$READER_OUT"
	READER=
}

# wait up to $2 s for path $1 to go away; sets WAITED
wait_gone() {
	local s=$SECONDS
	while [ -e "$1" ]; do
		((SECONDS - s >= $2)) && return 1
		sleep 0.2
	done
	WAITED=$((SECONDS - s))
}

# wait up to $2 s for a device of chip $1 (t6 / t2) to be bound again; sets WAITED
wait_dev() {
	local s=$SECONDS
	while :; do
		find_devs
		if [ "$1" = t6 ] && [ -n "$T6" ]; then break; fi
		if [ "$1" = t2 ] && [ -n "$T2" ]; then break; fi
		((SECONDS - s >= $2)) && return 1
		sleep 0.5
	done
	WAITED=$((SECONDS - s))
}

# ------------------------------------------------------------------ settings restore

ORIG_FORMAT= ORIG_ALL= ORIG_LOSSY=
restore() {
	[ -n "$READER" ] && kill "$READER" 2>/dev/null
	find_devs
	[ -n "$ORIG_FORMAT" ] && [ -n "$T6" ] && put "$T6/format" "$ORIG_FORMAT"
	[ -n "$ORIG_ALL" ] && [ -n "$T6" ] && put "$T6/all_modes" "$ORIG_ALL"
	[ -n "$ORIG_LOSSY" ] && [ -n "$T2" ] && put "$T2/lossy" "$ORIG_LOSSY"
	[ -n "${KEEP:-}" ] && kill "$KEEP" 2>/dev/null
}

# con2fbmap: current framebuffer of console $1
c2f_get() {
	local out
	out=$(sudo "$C2F" "$1" 2>/dev/null)
	[[ $out =~ ([0-9]+)[[:space:]]*$ ]] && printf '%s' "${BASH_REMATCH[1]}" || printf 0
}

# ------------------------------------------------------------------ steps
#
# Each step_X prints what to do, does its part and sets Q (the question) or
# SKIP (a reason not to ask). pre_X, if there is one, prints details before
# the run/skip question of a risky step.

step_sysfs() {
	local d
	for d in "$T6" "$T2"; do
		[ -n "$d" ] || continue
		show_dev "$d"
		check_mode "$d"
	done
	if [ -n "$T6" ] && [ "$(rd "$T6/usb_speed")" = usb2 ]; then
		say "  note: the T6 is on a USB 2 link: modes up to 2048 wide, smaller writes"
	fi
	say "Look: chip/model/connector right, mode equal to niri's, pixel_format set while on."
	Q="Do the files look right (and do the driver and niri agree on the modes)?"
}

step_cursor_edges() {
	say "On the T6 monitor: move the pointer slowly to each of the four edges and"
	say "corners, then a little past each edge onto the neighbouring monitor and back."
	say "On the tablet: the same, especially the LEFT and TOP edges."
	say "Look for: the cursor clipped cleanly at every edge, no copy of it appearing"
	say "at the opposite edge (wrap-around), no garbage block, no cursor left behind"
	say "after it moved to another monitor."
	pause "Done"
	Q="Was the cursor right at all edges of both displays?"
}

step_cursor_away() {
	say "Move the pointer onto another monitor and leave it there for 5 s, then"
	say "bring it back onto the T6 monitor. Hover over text (I-beam), a link (hand)"
	say "and a window border (resize arrows), on the T6 and on the tablet."
	say "Look for: the right shape every time, nothing stale or scrambled."
	pause "Done"
	Q="Did the cursor shape always match, on both displays?"
}

step_format() {
	local v
	ORIG_FORMAT=$(rd "$T6/format")
	say "Keep something moving on the T6 monitor for ~20 s: drag a window in"
	say "circles, or play a video there. The script switches format every 3 s."
	pause "Start moving, then press"
	for v in rgb24 yuv444 nv12 rgb32 auto; do
		put "$T6/format" "$v"
		sleep 1.5
		say "  format=$v, in use: $(rd "$T6/pixel_format")"
		sleep 1.5
	done
	put "$T6/format" "$ORIG_FORMAT"
	pause "Stop moving now, then press"
	sleep 1
	say "  at rest with format=$ORIG_FORMAT: in use $(rd "$T6/pixel_format") (auto: RGB24 or YUV444 where the mode allows)"
	say "  (a format in use different from the one asked for is fine: the driver"
	say "   falls back to NV12 where a format doesn't fit the mode)"
	ORIG_FORMAT=
	Q="Did the picture stay right through the switches (only colour precision changing), and is it sharp again now?"
}

step_lossy() {
	local v
	ORIG_LOSSY=$(rd "$T2/lossy")
	say "Keep a window moving on the tablet for ~15 s. The script changes lossy every 3 s."
	pause "Start moving, then press"
	for v in 0 2 8 0; do
		put "$T2/lossy" "$v"
		say "  lossy=$v"
		sleep 3
	done
	put "$T2/lossy" "$ORIG_LOSSY"
	ORIG_LOSSY=
	pause "Stop moving now, wait 1 s, then press"
	Q="Did the tablet keep updating, and is the picture exact (no streaks left) now that it rests?"
}

step_console_t2() {
	local fb old
	[ -n "$C2F" ] || { SKIP="con2fbmap not found (package fbset, or CON2FBMAP=...)"; return; }
	fb=$(fb_of "$(intf_of "$T2")")
	[ "$fb" != - ] || { SKIP="no framebuffer for the tablet"; return; }
	old=$(c2f_get "$TTY2")
	sudo "$C2F" "$TTY2" "$fb"
	say "  tty$TTY2 now on fb$fb (the tablet), was fb$old"
	say "Press Ctrl+Alt+F$TTY2. The tablet should show a text console (login prompt);"
	say "type a few letters. Stay 10 s. Then press Ctrl+Alt+F$NIRI_VT to get back to niri."
	pause "Back in niri? Then press"
	sudo "$C2F" "$TTY2" "$old"
	Q="Was the console on the tablet, and after the switch back are all outputs and the cursor right?"
}

step_all_modes() {
	local cur new before
	ORIG_ALL=$(rd "$T6/all_modes")
	new=$((1 - ORIG_ALL))
	before=$(rd "$T6/mode")
	niri_info "$(conn_of "$T6")"
	say "  all_modes=$ORIG_ALL: niri lists $NIRI_NMODES modes, current $NIRI_CUR"
	put "$T6/all_modes" "$new"
	sleep 3
	niri_info "$(conn_of "$T6")"
	say "  all_modes=$new: niri lists $NIRI_NMODES modes, current $NIRI_CUR, driver $(rd "$T6/mode")"
	put "$T6/all_modes" "$ORIG_ALL"
	sleep 3
	niri_info "$(conn_of "$T6")"
	cur=$(rd "$T6/mode")
	say "  all_modes=$ORIG_ALL again: niri lists $NIRI_NMODES modes, current $NIRI_CUR, driver $cur"
	ORIG_ALL=
	[ "$cur" = "$before" ] || say "  !! the mode changed: $before -> $cur"
	say "Expected: more modes with all_modes=1 (only if the monitor lists larger ones),"
	say "the same mode throughout, the monitor never blanking."
	Q="Did the mode list change as expected, without the monitor blanking?"
}

pre_console_t6() {
	niri_info "$(conn_of "$T6")"
	say "  niri's mode on the T6: $NIRI_CUR, the monitor's preferred: $NIRI_PREF"
	if [ "$NIRI_CUR" != "$NIRI_PREF" ]; then
		say "  They differ: the console uses the preferred mode, so each VT switch is a mode set."
	else
		say "  They are the same: the VT switch should not need a mode set."
	fi
}

step_console_t6() {
	local fb old
	[ -n "$C2F" ] || { SKIP="con2fbmap not found (package fbset, or CON2FBMAP=...)"; return; }
	fb=$(fb_of "$(intf_of "$T6")")
	[ "$fb" != - ] || { SKIP="no framebuffer for the T6"; return; }
	old=$(c2f_get "$TTY6")
	sudo "$C2F" "$TTY6" "$fb"
	say "  tty$TTY6 now on fb$fb (the T6), was fb$old; fb size $(rd "/sys/class/graphics/fb$fb/virtual_size")"
	say "Press Ctrl+Alt+F$TTY6. The T6 monitor should show a text console filling the"
	say "screen (not a small corner, no garbage); type a few letters, run 'ls' if you"
	say "log in. Stay 20 s (no blanking). Then press Ctrl+Alt+F$NIRI_VT to get back to niri."
	pause "Back in niri? Then press"
	sudo "$C2F" "$TTY6" "$old"
	check_mode "$T6"
	Q="Was the console right on the T6, and after the switch back are picture, mode and cursor right?"
}

step_unplug_t2() {
	reader_start "$T2"
	say "Drag a window around on the tablet (keep the mouse button down) and, while"
	say "it moves, pull the tablet's USB cable."
	if ! wait_gone "$T2/chip" 120; then
		say "  the tablet did not go away within 120 s"
		reader_stop
		Q="(The tablet was not unplugged.) Answer n, or s to skip."
		return
	fi
	say "  unplugged, driver unbound after ${WAITED} s"
	sleep 2
	say "Plug the tablet back in now (same port)."
	if wait_dev t2 120; then
		say "  tablet bound again after ${WAITED} s"
	else
		say "  !! no tablet after 120 s"
	fi
	sleep 3
	reader_stop
	[ -n "$T2" ] && check_mode "$T2"
	Q="Is the tablet's picture back (within ~5 s, up to ~45 s if the log says the device was busy), shown by niri, pen and brightness working?"
}

step_unplug_t6() {
	local conn fb fmt deffmt
	conn=$(conn_of "$T6")
	fb=$(fb_of "$(intf_of "$T6")")
	fmt=$(rd "$T6/format")
	deffmt=$(rd /sys/module/mct/parameters/format)
	niri_info "$conn"
	say "  before: $conn, fb$fb, format=$fmt, niri mode $NIRI_CUR"
	reader_start "$T6"
	say "Drag a window around on the T6 monitor and keep the pointer moving over it;"
	say "while it moves, pull the T6's USB cable (not the HDMI cable)."
	if ! wait_gone "$T6/chip" 120; then
		say "  the T6 did not go away within 120 s"
		reader_stop
		Q="(The T6 was not unplugged.) Answer n, or s to skip."
		return
	fi
	say "  unplugged, driver unbound after ${WAITED} s"
	sleep 3
	say "Plug the T6 back in now (same port)."
	if wait_dev t6 60; then
		say "  T6 bound again after ${WAITED} s"
	else
		say "  !! no T6 after 60 s"
	fi
	sleep 5
	reader_stop
	if [ -n "$T6" ]; then
		say "  after: $(conn_of "$T6"), fb$(fb_of "$(intf_of "$T6")"), format=$(rd "$T6/format") (module default $deffmt)"
		[ "$(conn_of "$T6")" = "$conn" ] || say "  note: the connector name changed; niri output settings by name won't apply"
		check_mode "$T6"
	fi
	say "Expected: picture back within ~5 s at the same mode, windows back on it."
	Q="Did the T6 come back right (picture, mode, cursor, niri output)?"
}

step_dpms() {
	local before
	before=$(rd "$T6/mode")
	say "  turning all monitors off through niri; don't touch anything for 10 s"
	niri msg action power-off-monitors
	sleep 10
	say "  T6 mode while off: $(rd "$T6/mode")"
	say "Now move the mouse or press a key to wake the monitors."
	pause "When all monitors show a picture again (or after 30 s), press"
	sleep 1
	check_mode "$T6"
	[ "$(rd "$T6/mode")" = "$before" ] || say "  !! mode before $before"
	t6_stats
	Q="Did every monitor come back, the T6 one at the same mode with the right picture and cursor?"
}

step_monitor_power() {
	local before
	before=$(rd "$T6/mode")
	t6_stats
	say "Move the pointer onto ANOTHER monitor and leave it there until asked."
	pause "Pointer away from the T6 monitor? Then press"
	say "Turn the T6 monitor off with its power button."
	pause "Off? Then wait here; press"
	sleep 8
	niri_info "$(conn_of "$T6")"
	say "  while off: driver mode $(rd "$T6/mode"), niri $NIRI_CUR"
	say "Turn the monitor back on."
	pause "Picture back (or 30 s passed)? Then press"
	sleep 2
	check_mode "$T6"
	[ "$(rd "$T6/mode")" = "$before" ] || say "  !! mode before $before (a smaller mode here can mean the EDID read failed while the monitor woke)"
	t6_stats
	say "Now move the pointer onto the T6 monitor and hover over text and a window border."
	pause "Done"
	Q="Picture back at the same mode, and the cursor's shape right the first time it entered the T6 monitor?"
}

step_suspend() {
	local m6= m2=
	[ -n "$T6" ] && m6=$(rd "$T6/mode")
	[ -n "$T2" ] && m2=$(rd "$T2/mode")
	say "Suspend the machine now (your usual key, or 'systemctl suspend' in another"
	say "terminal), wait ~15 s, wake it, unlock, and come back here."
	say "Optional: while it sleeps, pull the T6's HDMI cable and plug it back (a"
	say "monitor change during suspend)."
	pause "Resumed? Then press"
	sleep 3
	find_devs
	[ -n "$T6" ] && { check_mode "$T6"; [ "$(rd "$T6/mode")" = "$m6" ] || say "  !! T6 mode before $m6"; }
	[ -n "$T2" ] && { check_mode "$T2"; [ "$(rd "$T2/mode")" = "$m2" ] || say "  !! tablet mode before $m2"; }
	[ -n "$T6" ] && t6_stats
	Q="After resume: both displays right (picture, mode, cursor, tablet brightness), nothing missing in niri?"
}

STEPS=(sysfs cursor_edges cursor_away format lossy console_t2 all_modes console_t6
       unplug_t2 unplug_t6 dpms monitor_power suspend)
declare -A TITLE=(
	[sysfs]="sysfs files and modes"
	[cursor_edges]="cursor at every screen edge"
	[cursor_away]="cursor leaving and coming back, shape changes"
	[format]="T6 format switching under motion"
	[lossy]="tablet lossy switching under motion"
	[console_t2]="text console on the tablet, VT switch and back"
	[all_modes]="all_modes toggled"
	[console_t6]="text console on the T6, VT switch and back"
	[unplug_t2]="tablet USB unplug/replug during motion"
	[unplug_t6]="T6 USB unplug/replug during motion, sysfs read meanwhile"
	[dpms]="niri powers the monitors off and on (DPMS)"
	[monitor_power]="T6 monitor power button off/on"
	[suspend]="suspend and resume"
)
declare -A NEED=(
	[sysfs]=any [cursor_edges]=any [cursor_away]=any [format]=t6 [lossy]=t2
	[console_t2]=t2 [all_modes]=t6 [console_t6]=t6 [unplug_t2]=t2 [unplug_t6]=t6
	[dpms]=t6 [monitor_power]=t6 [suspend]=any
)
declare -A RISK=(
	[all_modes]="userspace re-probes the T6 monitor; niri should keep the mode, but if it doesn't, the monitor loses its signal briefly."
	[console_t6]="if niri's mode is not the monitor's preferred one, each VT switch is a mode set: the T6 monitor loses its signal twice."
	[unplug_t2]="needs a USB replug of the tablet; a cut frame can keep it dark for up to 45 s."
	[unplug_t6]="needs a USB replug of the T6: its monitor loses the signal and gets it back."
	[dpms]="all monitors lose their signal and get it back (the T6 monitor's menu may hang; recovery: power cycle it)."
	[monitor_power]="the T6 monitor is power cycled: signal off and on (its menu may hang; recovery: power cycle it again)."
	[suspend]="every monitor loses its signal and gets it back; the tablet re-runs its init."
)

# ------------------------------------------------------------------ main

trap restore EXIT
trap 'exit 130' INT
say "mct edge-case test, $(date), kernel $(uname -r)"
say "log: $LOG"
sudo -v || exit 1
(while sudo -n true 2> /dev/null; do sleep 50; done) &
KEEP=$!
command -v niri > /dev/null || say "warning: niri not found; mode comparisons will fail"
[ -n "$C2F" ] || say "note: con2fbmap not found: the console steps will be skipped (see the header)"

find_devs
[ -n "$T6$T2" ] || { say "no device bound to mct"; exit 1; }
say "T6: ${T6:-none}  connector $( [ -n "$T6" ] && conn_of "$T6")"
say "T2: ${T2:-none}  connector $( [ -n "$T2" ] && conn_of "$T2")"
say "niri runs on VT $NIRI_VT; consoles used: tty$TTY6 (T6), tty$TTY2 (tablet)"
logf "$(niri msg outputs 2>&1)"

declare -A RES NOTES LOGC
for i in "${!STEPS[@]}"; do
	s=${STEPS[$i]}
	n=$((i + 1))
	if ((n < START)); then
		RES[$s]="not run"
		continue
	fi
	find_devs
	case ${NEED[$s]} in
	t6) [ -n "$T6" ] || { RES[$s]="skipped (no T6)"; continue; } ;;
	t2) [ -n "$T2" ] || { RES[$s]="skipped (no tablet)"; continue; } ;;
	esac

	say ""
	say "=== $n/${#STEPS[@]}: ${TITLE[$s]}"
	if [ -n "${RISK[$s]:-}" ]; then
		say "RISKY: ${RISK[$s]}"
		declare -F "pre_$s" > /dev/null && "pre_$s"
		while :; do
			read -r -p "Run this step? [y/s=skip/q=quit]: " a
			case $a in y | s | q) break ;; esac
		done
		logf "> run: $a"
		[ "$a" = q ] && { RES[$s]="not run"; break; }
		[ "$a" = s ] && { RES[$s]=skipped; continue; }
	fi

	sudo -v
	log_mark
	Q= SKIP=
	"step_$s"
	if [ -n "$SKIP" ]; then
		say "  skipped: $SKIP"
		RES[$s]="skipped ($SKIP)"
		continue
	fi
	while :; do
		read -r -p "$Q [y/n/s=skip/q=quit]: " a
		case $a in y | n | s | q) break ;; esac
	done
	logf "> $Q $a"
	note=
	if [ "$a" = n ]; then
		read -r -p "What was wrong (one line, Enter for nothing): " note
		logf "> note: $note"
	fi
	sleep 1
	log_since
	LOGC[$s]="$LOGN log lines$( ((LOGBAD)) && echo ", $LOGBAD WARN/BUG")"
	case $a in
	y) RES[$s]=ok ;;
	n) RES[$s]=BAD ;;
	s) RES[$s]=skipped ;;
	q) RES[$s]="not answered"; break ;;
	esac
	NOTES[$s]=$note
done

say ""
say "summary:"
for i in "${!STEPS[@]}"; do
	s=${STEPS[$i]}
	line=$(printf '  %2d %-8s %s%s' "$((i + 1))" "${RES[$s]:-not run}" "${TITLE[$s]}" \
		"${LOGC[$s]:+ (${LOGC[$s]})}")
	say "$line"
	[ -n "${NOTES[$s]:-}" ] && say "       note: ${NOTES[$s]}"
done
say "full log: $LOG"

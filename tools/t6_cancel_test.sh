#!/usr/bin/env bash
# Which part makes the T6 drop off USB after a cut off transfer: the cancel
# request (0x14) or the cut transfer itself? Each step injects through
# debugfs mct_t6_inject, waits, reports from the kernel log and asks y/n.
set -u
res=()
step() {	# value, name
	local since dbg a
	since=$(date '+%Y-%m-%d %H:%M:%S')
	dbg=$(sudo sh -c 'dirname "$(ls /sys/kernel/debug/dri/*/mct_t6_stats | head -1)"')
	echo "== $2"
	sudo sh -c "echo $1 > $dbg/mct_t6_inject"
	sleep 6
	if journalctl -k --since "$since" --no-pager | grep -q "2-2: USB disconnect\|usb .*: USB disconnect.*" ; then
		echo "   kernel: the T6 dropped off USB and came back"
		d="dropped off"
	else
		echo "   kernel: no disconnect"
		d="stayed"
	fi
	journalctl -k --since "$since" --no-pager | grep -E "mct|disconnect" | sed 's/^/   /'
	while :; do
		read -r -p "   Move a window on the T6 monitor: picture right and updating? y/n + Enter: " a
		[ "$a" = y ] || [ "$a" = n ] && break
	done
	res+=("$2: device $d, picture $([ "$a" = y ] && echo ok || echo BAD)")
}
step 2 "cancel request only"
echo "If the picture is frozen now, unplug and replug the T6 USB cable before going on."
read -r -p "Enter to go on: " _
step 3 "cut transfer, no cancel"
printf '\nsummary:\n'; printf '  %s\n' "${res[@]}"

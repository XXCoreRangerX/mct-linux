#!/usr/bin/env bash
# Interactive checks for the T6 error recovery and hotplug handling. Each
# step waits for y/n; summary at the end. Needs the T6 bound to mct.
set -u
dbg=$(sudo sh -c 'dirname "$(ls /sys/kernel/debug/dri/*/mct_t6_stats | head -1)"')
stat() { sudo cat "$dbg/mct_t6_stats" | grep -E "^(recoveries|events)"; }
ask() {
	local a
	while :; do
		read -r -p "$1 y/n + Enter: " a
		[ "$a" = y ] || [ "$a" = n ] && break
	done
	res+=("$2: $([ "$a" = y ] && echo ok || echo BAD)")
}
res=()

echo "== 1: one transfer cut off halfway"
sudo sh -c "echo 1 > $dbg/mct_t6_inject"
sleep 1; stat
ask "Move a window on the T6 monitor. Picture still right and updating?" "cut transfer"

echo "== 2: three in a row"
for i in 1 2 3; do sudo sh -c "echo 1 > $dbg/mct_t6_inject"; sleep 0.5; done
sleep 1; stat
ask "Move a window again. Still right?" "three cut transfers"

echo "== 3: monitor off and on"
read -r -p "Turn the T6 monitor off with its power button, wait 5 s, turn it on. Enter when its picture is back (or after 30 s): " _
stat
ask "Picture back and right, at the same mode?" "monitor power cycle"

printf '\nsummary:\n'; printf '  %s\n' "${res[@]}"
echo; journalctl -k --since -10min --no-pager | grep -E "mct|transfer cancelled" | tail -8

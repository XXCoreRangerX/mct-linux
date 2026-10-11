#!/usr/bin/env bash
# Run a T6 userspace test: unbind the T6 from mct, run the script as root,
# bind it back (also on ctrl-c). Usage: tools/t6_run.sh tools/t6_rotate_test.py
set -eu
if [ "$(id -u)" != 0 ]; then
	py=$(nix build --no-link --print-out-paths --impure --expr \
		'with import <nixpkgs> {}; python3.withPackages (p: [ p.pyusb p.numpy ])')/bin/python3
	exec sudo PY="$py" "$0" "$@"
fi
drv=/sys/bus/usb/drivers/mct
intf=
for d in /sys/bus/usb/devices/*:1.0; do
	p=$(cat "$d/../idProduct" 2>/dev/null) || continue
	[ "$(cat "$d/../idVendor")" = 0711 ] && [ "${p#56}" != "$p" ] && intf=$(basename "$d")
done
[ -n "$intf" ] || { echo "no T6 found" >&2; exit 1; }
[ -e "$drv/$intf" ] && echo "$intf" > "$drv/unbind"
trap 'echo "$intf" > "$drv/bind" 2>/dev/null; echo "T6 bound to mct again"' EXIT
"$PY" "$@"

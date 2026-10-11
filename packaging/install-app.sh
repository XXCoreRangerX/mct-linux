#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
#
# Installs MCT Display Manager, mctctl and the mct udev rules from a release
# tarball (run from the unpacked directory):
#
#   sudo ./install.sh              install under /usr/local
#   sudo ./install.sh --uninstall  remove what install put there
#
# PREFIX changes /usr/local; DESTDIR installs into a staging root (packagers).
# The polkit policy and the udev rules go to /usr/share/polkit-1 and
# /etc/udev, where polkit and udev look for them, whatever PREFIX is.

set -eu

PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
here=$(cd "$(dirname "$0")" && pwd)

bin=$DESTDIR$PREFIX/bin
libexec=$DESTDIR$PREFIX/libexec
share=$DESTDIR$PREFIX/share
polkit=$DESTDIR/usr/share/polkit-1/actions
rules=$DESTDIR/etc/udev/rules.d

files="
$bin/mct-manager
$bin/mctctl
$libexec/mct-manager-helper
$share/applications/org.mctlinux.Manager.desktop
$share/icons/hicolor/scalable/apps/org.mctlinux.Manager.svg
$share/icons/hicolor/symbolic/apps/org.mctlinux.Manager-symbolic.svg
$polkit/org.mctlinux.manager.policy
$rules/70-mct.rules
$rules/71-mct-settings.rules
"

reload() {
	[ -z "$DESTDIR" ] || return 0
	if command -v udevadm >/dev/null 2>&1; then
		udevadm control --reload || true
	fi
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then
		gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" || true
	fi
}

if [ "${1:-}" = "--uninstall" ]; then
	for f in $files; do
		rm -f "$f"
	done
	reload
	echo "Removed MCT Display Manager, mctctl and the mct udev rules."
	exit 0
fi
[ $# -eq 0 ] || { echo "usage: $0 [--uninstall]" >&2; exit 2; }

install -Dm755 "$here/bin/mct-manager" "$here/bin/mctctl" -t "$bin"
install -Dm755 "$here/libexec/mct-manager-helper" -t "$libexec"
install -Dm644 "$here/share/applications/org.mctlinux.Manager.desktop" -t "$share/applications"
install -Dm644 "$here/share/icons/hicolor/scalable/apps/org.mctlinux.Manager.svg" \
	-t "$share/icons/hicolor/scalable/apps"
install -Dm644 "$here/share/icons/hicolor/symbolic/apps/org.mctlinux.Manager-symbolic.svg" \
	-t "$share/icons/hicolor/symbolic/apps"
mkdir -p "$polkit"
sed "s|@helper@|$PREFIX/libexec/mct-manager-helper|" \
	"$here/share/polkit-1/actions/org.mctlinux.manager.policy" > "$polkit/org.mctlinux.manager.policy"
chmod 644 "$polkit/org.mctlinux.manager.policy"
install -Dm644 "$here/udev/70-mct.rules" "$here/udev/71-mct-settings.rules" -t "$rules"
reload

echo "Installed under $PREFIX. To change settings without root, add your user"
echo "to group video (sudo usermod -aG video \$USER) and log in again."

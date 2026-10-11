#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
#
# Builds the release files into dist/ (used by .github/workflows/release.yml,
# works the same locally):
#
#   packaging/make-release.sh driver VERSION
#       dist/mct-dkms-VERSION.tar.gz: the module source with dkms.conf and
#       the udev rules, from the committed tree
#   packaging/make-release.sh app VERSION
#       dist/mct-app-VERSION-ARCH.tar.gz: mct-manager and mctctl built for
#       this machine (needs cargo, GTK 4.14 and libadwaita 1.5 development
#       files), with install.sh

set -eu

[ $# -eq 2 ] || { echo "usage: $0 driver|app VERSION" >&2; exit 2; }
what=$1
ver=$2
case $ver in
'' | *[!0-9A-Za-z.+~-]*) echo "invalid version: $ver" >&2; exit 2 ;;
esac

root=$(git rev-parse --show-toplevel)
dist=$root/dist
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$dist"

case $what in
driver)
	d=$tmp/mct-$ver
	mkdir -p "$d"
	git -C "$root" archive HEAD kernel LICENSE README.md CHANGELOG.md | tar -x -C "$d"
	sed "s/@VERSION@/$ver/" "$root/packaging/dkms.conf.in" > "$d/dkms.conf"
	mkdir -p "$d/udev"
	cp "$root"/packaging/udev/*.rules "$d/udev/"
	tar -C "$tmp" --owner=0 --group=0 -czf "$dist/mct-dkms-$ver.tar.gz" "mct-$ver"
	echo "$dist/mct-dkms-$ver.tar.gz"
	;;
app)
	arch=$(uname -m)
	cargo build --release --locked --manifest-path "$root/app/Cargo.toml"
	a=$tmp/mct-app-$ver-$arch
	data=$root/app/data
	install -Dm755 "$root/app/target/release/mct-manager" "$root/app/target/release/mctctl" -t "$a/bin"
	install -Dm755 "$data/mct-manager-helper" -t "$a/libexec"
	install -Dm644 "$data/org.mctlinux.Manager.desktop" -t "$a/share/applications"
	install -Dm644 "$data/icons/hicolor/scalable/apps/org.mctlinux.Manager.svg" \
		-t "$a/share/icons/hicolor/scalable/apps"
	install -Dm644 "$data/icons/hicolor/symbolic/apps/org.mctlinux.Manager-symbolic.svg" \
		-t "$a/share/icons/hicolor/symbolic/apps"
	install -Dm644 "$data/org.mctlinux.manager.policy" -t "$a/share/polkit-1/actions"
	install -Dm644 "$root"/packaging/udev/*.rules -t "$a/udev"
	install -Dm755 "$root/packaging/install-app.sh" "$a/install.sh"
	install -Dm644 "$root/app/README.md" "$root/LICENSE" -t "$a"
	strip "$a/bin/mct-manager" "$a/bin/mctctl" 2>/dev/null || true
	tar -C "$tmp" --owner=0 --group=0 -czf "$dist/mct-app-$ver-$arch.tar.gz" "mct-app-$ver-$arch"
	echo "$dist/mct-app-$ver-$arch.tar.gz"
	;;
*)
	echo "usage: $0 driver|app VERSION" >&2
	exit 2
	;;
esac

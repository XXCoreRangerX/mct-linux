#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Win11 capture VM for the vendor driver (only needed to re-capture USB traffic).
# The tablet is attached through usb-redir on 127.0.0.1:32001; start
#   usbredirserver -p 32002 0711:520d  and  tools/tee.py captures/redirN.bin
# first (see docs/capture.md). QMP socket: vm/qmp.sock (scriptable: send-key, screendump, ...).
#
# Usage: WIN_ISO=/path/to/Win11.iso DRIVER_ISO=/path/to/driver-t2.iso vm/runvm.sh
set -eu
cd "$(dirname "$0")"
: "${WIN_ISO:?set WIN_ISO to a Windows 11 x64 ISO}"
: "${DRIVER_ISO:?set DRIVER_ISO to an ISO with autounattend.xml, install-t2.cmd and t2/ (vendor driver files)}"
OVMF=${OVMF:-$(nix build --no-link --print-out-paths nixpkgs#OVMF.fd)/FV}
[ -f win11.qcow2 ] || qemu-img create -f qcow2 win11.qcow2 40G
[ -f OVMF_VARS.fd ] || install -m 644 "$OVMF/OVMF_VARS.fd" OVMF_VARS.fd
exec qemu-system-x86_64 -enable-kvm -name mctcap \
  -m 6144 -smp 4 -cpu host -machine q35 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF/OVMF_CODE.fd" \
  -drive if=pflash,format=raw,file=OVMF_VARS.fd \
  -drive file=win11.qcow2,if=none,id=disk0,format=qcow2 \
  -device ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0 \
  -cdrom "$WIN_ISO" -drive file="$DRIVER_ISO",media=cdrom,readonly=on \
  -device usb-ehci,id=ehci \
  -chardev socket,id=usbredir0,host=127.0.0.1,port=32001,reconnect-ms=2000 \
  -device usb-redir,chardev=usbredir0,id=usbredir0,bus=ehci.0 \
  -qmp unix:qmp.sock,server=on,wait=off \
  -vga std -display gtk \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -boot order=d

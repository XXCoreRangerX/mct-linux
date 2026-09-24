# USB capture setup (what worked)

## VM

- Host: NixOS, QEMU/KVM (`/dev/kvm` world-writable), 6 GB RAM / 4 vCPU / q35 /
  OVMF (plain, `OVMF_CODE.fd` + writable `OVMF_VARS` copy), 40 GB SATA qcow2,
  e1000, std VGA, GTK display. See `vm/runvm.sh`.
- Guest: Win11 25H2 consumer EN-US x64 ISO.
  No TPM/SwTPM: LabConfig bypasses (`BypassTPMCheck/SecureBootCheck/RAM/CPU`)
  + `BypassNRO` in `vm/autounattend.xml` (GPT layout, Pro image,
  generic key, local user `mct`, autologon).
- Driver autoinstall: a small ISO (`mkisofs`) holding
  `autounattend.xml` (root), `install-t2.cmd`, `t2/TrgIdd64.inf`,
  `t2/TrgIdd.dll`, `t2/T2Indisp.dll`, `t2/trgidd64.cat` (from MCT's "Trigger
  Family" driver package, not redistributed here); FirstLogonCommands runs
  `pnputil /add-driver ... /install`. The first capture passed the tablet via
  `-device usb-host,...`; later ones use usb-redir (below).
- Result: driver binds on first logon, desktop extends, cursor visible on
  tablet. Full install ~30-40 min on emulated SATA.

## Capture

- `sudo modprobe usbmon`, then run the capture **as root** (debugfs ignores
  chmod): `sudo sh -c 'cat /sys/kernel/debug/usb/usbmon/6u > cap.bin' &`
- Kernel 7.2 usbmon here is **text-only** (`0u` too) and **truncates every
  transfer at ~32 B shown**, enough for all short commands/headers, NOT for
  frame payloads or the 43/62 B tails. Bulk DATA presence: headers only.
- Text analysis was ad-hoc `grep`/`awk` (see init doc for the recipe).
- Bulk starts at first `Bo:6:011`; OUT/IN pairing mandatory; isoc flood from
  bus-6 audio dominates file size (400 MB total, 1262 tablet lines).

## Captures in this repo

- `captures/redir2.bin`: full usbredir capture of the Windows driver's init and
  first (black) frames. Source of `t2d/init.bin` (`tools/gen_init_blob.py`).
- `captures/frame1_rle.bin`: one captured black `0x13` frame (encoder test vector).
- Not published: the usbmon text capture (`cap_install.bin`, 750 MB) and
  `redir4.bin` (desktop session incl. partial updates; it contains screen
  content). Findings from them are written up in the docs.

## Full-fidelity capture via usbredir (used for redir2/redir4)

usbmon text truncates at ~32 B/transfer, so use a TCP tee instead (all userspace):

```bash
usbredirserver -p 32002 0711:520d &          # usbredir >= 0.10 (nixpkgs: usbredir)
python3 tools/tee.py captures/redirN.bin &      # listens 32001 -> 32002
# boot: vm/runvm.sh (usb-redir chardev on 32001), or hot-attach to a running VM
# in QEMU monitor (GTK: Ctrl+Alt+G, View -> compatmonitor0):
#   chardev-add socket,id=urN,host=127.0.0.1,port=32001
#   device_add usb-redir,chardev=urN,id=urN,bus=ehci.0
python3 tools/parse_redir.py captures/redirN.bin   # per-EP streams
```

`tools/redir_ops.py` loads a capture as ordered per-transfer OUT ops (keeps
transfer boundaries, which the firmware requires) + `frames()` for `0x13`.
Killing usbredirserver drops the tablet out of scanout; power-cycle + re-init after.
VM login (from `vm/autounattend.xml`): `mct` / `(password not published)`. `vm/runvm.sh` exposes a QMP
socket (`vm/qmp.sock`) for scripting (`send-key`, `screendump`, `device_add`).

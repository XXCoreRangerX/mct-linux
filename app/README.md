<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com> -->

# MCT Display Manager

Companion app for the mct driver: settings and status for MCT Trigger USB
displays (Trigger 6 HDMI adapters, the Wacom DTU-1031X). It talks to the
driver's sysfs files ([companion-app.md](../docs/driver/companion-app.md), [ABI-sysfs-mct.txt](../docs/driver/ABI-sysfs-mct.txt)).

- `mct-manager`: a GTK 4 / libadwaita window, one page per display
- `mct-manager --tray`: the same with a tray icon (StatusNotifierItem); the
  window opens from the tray menu, closing it keeps the tray running
- `mctctl`: the command line version
- `mct-manager-helper`: switches the tablet between the kernel driver and
  the legacy daemon (wacom-t2d over EVDI); run as root through pkexec

Displays are found once a second, so hotplug shows up without udev.

## Building

Needs Rust 1.85 or later, GTK 4.14 or later, libadwaita 1.5 or later and
pkg-config.

```sh
cargo build --release
cargo build --release --no-default-features   # mctctl only, no GTK
```

With Nix, from the repository root:

```sh
nix build .#mct-app
```

Installing by hand (paths for a /usr prefix):

```sh
install -Dm755 target/release/mct-manager target/release/mctctl -t /usr/bin
install -Dm755 data/mct-manager-helper -t /usr/libexec
install -Dm644 data/org.mctlinux.Manager.desktop -t /usr/share/applications
install -Dm644 data/icons/hicolor/scalable/apps/org.mctlinux.Manager.svg \
    -t /usr/share/icons/hicolor/scalable/apps
install -Dm644 data/icons/hicolor/symbolic/apps/org.mctlinux.Manager-symbolic.svg \
    -t /usr/share/icons/hicolor/symbolic/apps
sed 's|@helper@|/usr/libexec/mct-manager-helper|' data/org.mctlinux.manager.policy \
    > /usr/share/polkit-1/actions/org.mctlinux.manager.policy
```

The app looks for the helper in `<prefix>/libexec` next to its own binary,
then in PATH.

## Permissions

Reading needs nothing. Changing settings needs write access to the
device's files: a udev rule makes them group `video` and group-writable,
and your user has to be in group `video` (log in again after adding it).
The NixOS module installs the rules; elsewhere add them, for example as
/etc/udev/rules.d/71-mct-settings.rules:

```udev
ACTION=="bind", SUBSYSTEM=="usb", DRIVER=="mct", RUN+="/bin/sh -c 'cd /sys%p/mct && for f in format all_modes lossy; do [ -e $f ] && chgrp video $f && chmod g+w $f; done'"
ACTION=="add", SUBSYSTEM=="backlight", KERNEL=="mct_t2-*", RUN+="/bin/sh -c 'chgrp video /sys%p/brightness && chmod g+w /sys%p/brightness'"
```

Without them the window shows the settings as read-only with this hint, and
mctctl exits with status 3. On the real /sys, brightness also falls back to
logind (`SetBrightness`), which works for the active session without a rule.

Switching the tablet's driver asks for an administrator password (polkit
action org.mctlinux.manager.helper). On NixOS the permanent choice is
`hardware.mct.driver = "kernel"` or `"evdi"`.

## Saved settings

The driver starts every display with the module's defaults, also after a
replug or a T6 restart. Settings changed in the window, the tray or with
`mctctl set` are remembered in `~/.config/mct/settings.toml`, and the app
(window or tray) writes them back whenever a remembered display appears;
`mctctl apply` does it by hand. Displays are recognised by a serial number
where they have one (the DTU-1031X by its pen's), otherwise by the USB
port: a T6 adapter in another port starts with the defaults.

## mctctl

```text
mctctl list [-v|-vv]               displays, their driver and notes
mctctl info [-v|-vv] [DEV]         settings and state of one display;
                                   -v adds product, chip and firmware,
                                   USB, driver, monitor (EDID), pen;
                                   -vv also the adapter's internals, USB
                                   descriptors and endpoints, the decoded
                                   and raw EDID, driver details
mctctl get [DEV] KEY               one value
mctctl set [DEV] KEY VALUE         format, all_modes, lossy, brightness
                                   (remembered; --temporary to not save)
mctctl apply [DEV]                 apply the saved settings now
mctctl forget [DEV]                drop the saved settings of a display
mctctl saved                       list the saved settings
mctctl stats [DEV] [--watch]       update rate, data rate, errors
mctctl driver [DEV] kernel|legacy  switch the tablet's driver
```

DEV is an interface (`2-2:1.0`), a connector (`HDMI-A-3` or
`card2-HDMI-A-3`) or an index from `mctctl list`; with one display it can
be left out. `list`, `info` and `stats` take `--json` (`stats --watch
--json` prints one object per line). Exit status: 0 ok, 1 error, 2 invalid
input, 3 permission denied.

```console
$ mctctl list
#   INTERFACE  CHIP  MODEL            CONNECTOR  MODE          FORMAT        DRIVER
0   2-2:1.0    t6    j5create JUA254  HDMI-A-3   3440x1440@60  auto (RGB24)  mct
1   3-1:1.0    t2    Wacom DTU-1031X  USB-1      1024x600@60   lossy 0       mct
2!  4-1:1.0    t6    T6 USB Station   HDMI-A-4   off           nv12          mct
2! HDMI-A-4: Connected over USB 2: modes are limited to 2048 pixels wide and motion is slower. ...
$ mctctl set HDMI-A-3 format nv12
$ mctctl set USB-1 brightness 0
```

## Tray

`mct-manager --tray` shows each display with its mode and format, quick
settings (format and "Show all available modes" for a T6, brightness and
lossy level for the tablet), "Open MCT Display Manager" and "Quit". Bars
that host StatusNotifierItems draw it (KDE, Waybar, Noctalia; GNOME needs
the AppIndicator extension). If the bar starts after the app, as at login,
the icon appears once the bar is up; starting `mct-manager` again opens the
window. To start it with the session, put a copy of the .desktop file with
`Exec=mct-manager --tray` into `~/.config/autostart` (NixOS:
`hardware.mct.app.tray = true`).

## Testing without a device

`MCT_SYSFS_ROOT` moves the root that `sys/` and `proc/` are read from
(default `/`). The tests build a fake tree with two T6 adapters and the
tablet:

```sh
cargo test
cargo run --example fake_sysfs -- /tmp/mct-fake
MCT_SYSFS_ROOT=/tmp/mct-fake cargo run --bin mct-manager
MCT_SYSFS_ROOT=/tmp/mct-fake cargo run --bin mctctl -- list
```

Switching drivers is refused while `MCT_SYSFS_ROOT` is set.

# Installing and building

## From a release

Each [release](https://github.com/XXCoreRangerX/mct-linux/releases) has the
driver as DKMS source (built on your machine for every installed kernel, 6.12
or later) and MCT Display Manager with `mctctl` for x86_64 and aarch64:

```sh
sudo tar -C /usr/src -xzf mct-dkms-VERSION.tar.gz
sudo dkms install mct/VERSION
sudo modprobe mct

tar -xzf mct-app-VERSION-x86_64.tar.gz && cd mct-app-VERSION-x86_64
sudo ./install.sh        # app, polkit policy, udev rules; --uninstall removes them
```

The app needs GTK 4.14 and libadwaita 1.5 or later (Ubuntu 24.04, Fedora
40, Arch and newer).

## From source

Needs Linux 6.12 or later with `DRM`, `DRM_KMS_HELPER`,
`DRM_GEM_SHMEM_HELPER`, `BACKLIGHT_CLASS_DEVICE` and `USB` enabled, and the
kernel's headers or build tree.

```sh
make -C kernel KDIR=/lib/modules/$(uname -r)/build
sudo make -C kernel install
sudo depmod -a
sudo modprobe mct
```

- `MCT_T2=n` or `MCT_T6=n` on the make command line leaves a backend out.
- The driver binds to supported devices automatically. The T6 shows up as an
  `HDMI-A-n` connector, the tablet as `USB-n`.
- With Secure Boot, the module has to be signed with a key the kernel
  trusts.
- `modprobe` also loads the DRM helper modules mct needs. A plain
  `insmod kernel/mct.ko` only works once those are loaded (otherwise it fails
  with "Unknown symbol").
- To reload after a rebuild: unbind the devices (`echo <interface> >
  /sys/bus/usb/drivers/mct/unbind` for each entry there), then `rmmod mct`
  and `modprobe mct`.

The app: `cargo build --release` in `app/` (see
[app/README.md](../app/README.md)).

### Older kernels

The driver is written for the DRM API of Linux 7.2 and mainly tested there.
On 6.12 to 7.1, `kernel/Kbuild` detects which newer interfaces the kernel
lacks and `kernel/mct_compat.h` maps them onto the older ones (the renamed
atomic state, the vblank timer helpers, the fbdev setup, the DMA device for
buffer imports, the async plane check). On 7.2 and later none of it is used.
CI builds for 6.12 and 6.18; 6.12 was tested in a VM with the JUA254.

### Access for the tools

The scripts in `tools/` open the devices directly. Give your user access
with a udev rule such as this one (the NixOS module installs it):

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="5201|5209|520d|520f|5600|5601", TAG+="uaccess"
```

## NixOS

Plain `configuration.nix`:

```nix
imports = [ /path/to/mct-linux/nix/nixos.nix ];
hardware.mct.enable = true;
```

With flakes, add this repository as an input and import
`nixosModules.default`. The module builds the driver for the system's own
kernel (`boot.kernelPackages`); the flake's `packages.x86_64-linux.mct-kmod`
is built for `linuxPackages_latest`. The module also installs `mct-reload`,
which unbinds the devices and loads the module again after a rebuild,
without a reboot.

| Option (`hardware.mct.*`) | Meaning |
|---|---|
| `enable` | build and load the driver |
| `format`, `allModes`, `lossy` | defaults for new devices (see [usage.md](usage.md#settings)) |
| `driver = "evdi"` | the legacy EVDI + wacom-t2d route for the tablet instead of the kernel driver |
| `consoleMap` | text consoles on the tablet (adds `fbcon=map:...`, e.g. `"01"` for every second VT) |
| `niriFix` | build niri with the patches in `nix/`: clean up the output of a removed USB display; render USB displays into system memory, which makes reading frames many times faster |
| `niriImmediateCursor` | with `niriFix`: also the niri and smithay patches that move the cursor at once instead of with the next frame ([why](research/cursor-latency.md)). `NIRI_IMMEDIATE_CURSOR=0` turns it off at run time |
| `app.enable`, `app.tray` | install MCT Display Manager and `mctctl`; start the tray icon with the session |


## Installing

**Driver** (Linux 7.2 or later, with DKMS):

```sh
sudo tar -C /usr/src -xzf mct-dkms-@VERSION@.tar.gz
sudo dkms install mct/@VERSION@
sudo modprobe mct
```

DKMS rebuilds the module for each new kernel. NixOS users import the flake's
NixOS module instead (see the README).

**MCT Display Manager and mctctl** (GTK 4.14 and libadwaita 1.5 or later):

```sh
tar -xzf mct-app-@VERSION@-x86_64.tar.gz   # or -aarch64
cd mct-app-@VERSION@-x86_64
sudo ./install.sh
```

`install.sh` also installs the udev rules (`--uninstall` removes everything).
Add your user to group `video` to change settings without root.
`SHA256SUMS` lists the checksums of the files.

# SPDX-License-Identifier: GPL-2.0-only
# Non-flake entry point for the NixOS module:
#   imports = [ /path/to/mct-linux/nix/nixos.nix ];
#   hardware.mct.enable = true;
import ./module.nix { self = null; }

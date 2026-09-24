# Non-flake entry point for the NixOS module:
#   imports = [ /path/to/mct-t2-linux/nix/nixos.nix ];
#   hardware.mct-t2.enable = true;
import ./module.nix { self = null; }

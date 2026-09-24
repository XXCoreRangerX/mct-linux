# Build mct_t2 against the *running system's* nixpkgs kernel (for local testing):
#   nix-build nix/kmod-local.nix -o result-kmod
let pkgs = import <nixpkgs> { }; in
pkgs.callPackage ./kmod.nix { kernel = pkgs.linuxPackages_latest.kernel; }

# SPDX-License-Identifier: GPL-2.0-only
{
  description = "Linux support for MCT Trigger USB displays (Trigger II, Trigger 6)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      kernelPackages = pkgs.linuxPackages_latest;
      evdi = pkgs.callPackage ./nix/evdi.nix { inherit kernelPackages; };
    in
    {
      packages.${system} = {
        inherit evdi;
        wacom-t2d = pkgs.callPackage ./nix/wacom-t2d.nix { inherit evdi; };
        mct-kmod = pkgs.callPackage ./nix/kmod.nix { inherit (kernelPackages) kernel; };
        mct-app = pkgs.callPackage ./nix/mct-app.nix { };
        default = self.packages.${system}.mct-kmod;
      };

      nixosModules.default = import ./nix/module.nix { inherit self; };

      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.gcc pkgs.gnumake pkgs.pkg-config pkgs.libusb1
          (pkgs.python3.withPackages (ps: [ ps.pyusb ps.numpy ps.pillow ps.capstone ]))
        ];
      };
    };
}

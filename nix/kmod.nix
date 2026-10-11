# SPDX-License-Identifier: GPL-2.0-only
# mct DRM kernel module (Trigger II and Trigger 6), built against the given kernelPackages.
{ lib, stdenv, kernel }:
stdenv.mkDerivation {
  pname = "mct";
  version = "0.5.0-${kernel.version}";
  src = ../kernel;
  nativeBuildInputs = kernel.moduleBuildDependencies;
  # toolchain flags only; drop the in-tree O=/--eval flags
  makeFlags = lib.filter (f: !(lib.hasPrefix "O=" f || lib.hasPrefix "--eval" f)) kernel.makeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=${placeholder "out"}"
  ];
  installTargets = [ "install" ];
  meta = {
    description = "DRM driver for MCT Trigger USB displays (Trigger II, Trigger 6)";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}

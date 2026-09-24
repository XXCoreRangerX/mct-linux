# mct_t2 DRM kernel module, built against the given kernelPackages.
{ lib, stdenv, kernel }:
stdenv.mkDerivation {
  pname = "mct-t2";
  version = "0.3.2-${kernel.version}";
  src = ../kernel;
  nativeBuildInputs = kernel.moduleBuildDependencies;
  # toolchain flags only; drop the in-tree O=/--eval flags
  makeFlags = lib.filter (f: !(lib.hasPrefix "O=" f || lib.hasPrefix "--eval" f)) kernel.makeFlags ++ [
    "KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    "INSTALL_MOD_PATH=${placeholder "out"}"
  ];
  installTargets = [ "install" ];
  meta = {
    description = "DRM driver for MCT Trigger II USB displays (Wacom DTU-1031X)";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}

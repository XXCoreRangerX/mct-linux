# mct_t2 DRM kernel module, built against the given kernelPackages.
{ lib, stdenv, kernel }:
stdenv.mkDerivation {
  pname = "mct-t2";
<<<<<<< HEAD
  version = "0.3.11-${kernel.version}";
||||||| baae411 (kernel: skip the lossless attempt during a streak of heavy updates)
  version = "0.3.10-${kernel.version}";
=======
  version = "0.3.9-${kernel.version}";
>>>>>>> parent of baae411 (kernel: skip the lossless attempt during a streak of heavy updates)
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

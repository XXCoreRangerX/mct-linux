# SPDX-License-Identifier: GPL-2.0-only
{ lib, stdenv, pkg-config, libusb1, evdi }:
stdenv.mkDerivation {
  pname = "wacom-t2d";
  version = "0.1.0";
  src = ../wacom-t2d;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libusb1 evdi ];
  makeFlags = [
    "PREFIX=${placeholder "out"}"
    "EVDI_INC=${evdi.src}/library"
    "EVDI_LIB=${evdi}/lib"
  ];
  meta = {
    description = "Userspace display daemon for the Wacom DTU-1031X (MCT Trigger II), on top of EVDI";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
    mainProgram = "wacom-t2d";
  };
}

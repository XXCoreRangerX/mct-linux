# SPDX-License-Identifier: GPL-2.0-only
{ lib, stdenv, pkg-config, libusb1, evdi }:
stdenv.mkDerivation {
  pname = "t2d";
  version = "0.1.0";
  src = ../t2d;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libusb1 evdi ];
  makeFlags = [
    "PREFIX=${placeholder "out"}"
    "EVDI_INC=${evdi.src}/library"
    "EVDI_LIB=${evdi}/lib"
  ];
  meta = {
    description = "Userspace display daemon for MCT Trigger II (T2) USB displays, e.g. Wacom DTU-1031X";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
    mainProgram = "t2d";
  };
}

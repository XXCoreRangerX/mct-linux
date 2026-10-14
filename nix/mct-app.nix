# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>
# The companion app: mct-manager (window and tray), mctctl, and the root
# helper for switching the tablet's driver (run through pkexec).
{ lib, rustPlatform, pkg-config, wrapGAppsHook4, gtk4, libadwaita, kmod }:
rustPlatform.buildRustPackage {
  pname = "mct-app";
  version = "0.6.1";
  src = lib.cleanSourceWith {
    src = ../app;
    filter = path: type: baseNameOf path != "target";
  };
  cargoHash = "sha256-UAzvzCs1SBCD6yaetI4OzPxw547hcci3pb89GesTgOQ=";

  nativeBuildInputs = [ pkg-config wrapGAppsHook4 ];
  buildInputs = [ gtk4 libadwaita ];

  # only the window needs the GTK environment; the helper runs as root
  # through pkexec and must stay a plain script
  dontWrapGApps = true;

  postInstall = ''
    install -Dm644 data/org.mctlinux.Manager.desktop -t $out/share/applications
    install -Dm644 data/icons/hicolor/scalable/apps/org.mctlinux.Manager.svg \
      -t $out/share/icons/hicolor/scalable/apps
    install -Dm644 data/icons/hicolor/symbolic/apps/org.mctlinux.Manager-symbolic.svg \
      -t $out/share/icons/hicolor/symbolic/apps

    install -Dm755 data/mct-manager-helper -t $out/libexec
    substituteInPlace $out/libexec/mct-manager-helper \
      --replace-fail 'modprobe=modprobe' 'modprobe=${kmod}/bin/modprobe'
    install -Dm644 data/org.mctlinux.manager.policy -t $out/share/polkit-1/actions
    substituteInPlace $out/share/polkit-1/actions/org.mctlinux.manager.policy \
      --replace-fail '@helper@' "$out/libexec/mct-manager-helper"
  '';

  postFixup = ''
    wrapGApp $out/bin/mct-manager
  '';

  meta = {
    description = "Settings and status for MCT USB displays (mct driver): window, tray and mctctl";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
    mainProgram = "mct-manager";
  };
}

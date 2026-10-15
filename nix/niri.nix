# SPDX-License-Identifier: GPL-2.0-only
# niri with the patches in this directory (see the niriFix and
# niriImmediateCursor options in module.nix):
# - niri-device-removed-fix.patch, niri-display-only-sysmem.patch
# - with immediateCursor: niri-immediate-cursor.patch, and
#   smithay-immediate-cursor.patch for the smithay niri builds with.
#   smithay is a git dependency, so it is patched in the build's writable
#   copy of the vendored crates (git crates carry no file checksums).
{ lib, niri, immediateCursor ? true }:

niri.overrideAttrs (o: {
  patches = (o.patches or [ ]) ++ [
    ./niri-device-removed-fix.patch
    ./niri-display-only-sysmem.patch
  ] ++ lib.optional immediateCursor ./niri-immediate-cursor.patch;

  postPatch = (o.postPatch or "") + lib.optionalString immediateCursor ''
    smithay=("$cargoDepsCopy"/source-git-*/smithay-0.7.0)
    if [ ! -d "''${smithay[0]}" ]; then
      echo "niri.nix: no vendored smithay-0.7.0 in $cargoDepsCopy" >&2
      exit 1
    fi
    patch -d "''${smithay[0]}" -p1 < ${./smithay-immediate-cursor.patch}
  '';
})

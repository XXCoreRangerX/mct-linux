# EVDI built for kernels >= 7.2, which renamed struct drm_atomic_state -> drm_atomic_commit
# (and drm_atomic_state_{alloc,clear,put} -> drm_atomic_commit_*). EVDI 1.15.1
# already builds; the rename is a no-op there and only matters for 1.14.x
# (same change as nix/evdi-linux-7.2.patch).
{ lib, kernelPackages }:
kernelPackages.evdi.overrideAttrs (o: {
  postPatch = (o.postPatch or "") + lib.optionalString (lib.versionAtLeast kernelPackages.kernel.version "7.2") ''
    sed -i -e 's/struct drm_atomic_state\b/struct drm_atomic_commit/g' \
           -e 's/drm_atomic_state_\(alloc\|clear\|put\)/drm_atomic_commit_\1/g' \
           module/*.c module/*.h
  '';
  # nixpkgs installs only libevdi.so, but its SONAME is libevdi.so.1
  postInstall = (o.postInstall or "") + ''
    [ -e $out/lib/libevdi.so.1 ] || ln -s libevdi.so $out/lib/libevdi.so.1
  '';
})

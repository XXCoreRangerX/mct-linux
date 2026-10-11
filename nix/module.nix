# NixOS module: USB displays with MCT Trigger chips (Trigger II, Trigger 6) as real monitors.
{ self }:
{ config, lib, pkgs, ... }:
let
  cfg = config.hardware.mct;
  kernelPackages = config.boot.kernelPackages;
  kmod = pkgs.callPackage ./kmod.nix { inherit (kernelPackages) kernel; };
  evdi = pkgs.callPackage ./evdi.nix { inherit kernelPackages; };
  t2d = pkgs.callPackage ./t2d.nix { inherit evdi; };
in
{
  # the old option names (from when only the Trigger II was supported) keep working
  imports = map (o: lib.mkRenamedOptionModule [ "hardware" "mct-t2" o ] [ "hardware" "mct" o ]) [
    "enable" "consoleMap" "niriFix" "lossy" "format" "driver"
  ];

  options.hardware.mct = {
    enable = lib.mkEnableOption "support for USB displays with MCT Trigger chips";
    consoleMap = lib.mkOption {
      type = lib.types.nullOr lib.types.str;
      default = null;
      example = "01";
      description = ''
        Value for the fbcon=map: kernel parameter, one framebuffer index per
        console, repeated. With the GPU on fb0 and the tablet on fb1, "01"
        puts tty2, tty4, ... on the tablet and "1" moves every console there.
        fbcon applies it when the tablet's framebuffer registers during boot.
      '';
    };
    niriFix = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Build niri with two patches for USB displays:
        nix/niri-device-removed-fix.patch: niri 26.04 fails to clean up a
        removed DRM device ("error creating DrmNode"), which leaves stale
        outputs behind after unplugging a USB display.
        nix/niri-display-only-sysmem.patch: render frames for display-only
        devices into buffers allocated on the device itself (system memory)
        instead of the primary GPU's VRAM, so drivers that read frames with
        the CPU don't read over PCIe (about 10x faster). Falls back to the
        old behaviour if the GPU can't render into them;
        NIRI_DISPLAY_ONLY_PRIMARY_ALLOC=1 forces the old behaviour.
      '';
    };
    lossy = lib.mkOption {
      type = lib.types.ints.between 0 8;
      default = 0;
      description = ''
        Maximum tolerance for lossy updates when a lossless update would not
        fit into one 60 Hz frame (heavy video, photo scrolling). The driver
        uses 1 or 2 when that is enough and this value only for updates far
        over budget; the picture is refined to lossless about 150 ms after
        motion stops. 0 = always lossless. 4 matches the vendor driver.
        Trigger II (Wacom DTU-1031X) only. Can be changed at runtime through
        /sys/module/mct/parameters/lossy.
      '';
    };
    format = lib.mkOption {
      type = lib.types.enum [ "auto" "rgb24" "rgb32" "yuv444" "nv12" ];
      default = "auto";
      description = ''
        Pixel format for Trigger 6 adapters. "auto": exact RGB (or YUV 4:4:4
        where RGB exceeds the adapter's memory bandwidth) while the screen is
        still, NV12 during large changes. The others fix the format; full
        colour formats still fall back to NV12 at modes the adapter can't
        scan them out at. Can be changed at runtime through
        /sys/module/mct/parameters/format.
      '';
    };
    driver = lib.mkOption {
      type = lib.types.enum [ "kernel" "evdi" ];
      default = "kernel";
      description = ''
        "kernel": native DRM driver (module mct; works on the text console too).
        "evdi": EVDI virtual connector + t2d userspace daemon.
      '';
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    (lib.mkIf cfg.niriFix {
      programs.niri.package = pkgs.niri.overrideAttrs (o: {
        patches = (o.patches or [ ]) ++ [
          ./niri-device-removed-fix.patch
          ./niri-display-only-sysmem.patch
        ];
      });
    })

    (lib.mkIf (cfg.driver == "kernel") {
      boot.extraModulePackages = [ kmod ];
      boot.extraModprobeConfig = "options mct lossy=${toString cfg.lossy} format=${cfg.format}";
      # swap in the module of the current generation without rebooting
      environment.systemPackages =
        let
          reload = pkgs.writeShellScriptBin "mct-reload" ''
            set -eu
            [ "$(id -u)" = 0 ] || exec sudo "$0" "$@"
            for intf in /sys/bus/usb/drivers/mct/*:*; do
              [ -e "$intf" ] && basename "$intf" > /sys/bus/usb/drivers/mct/unbind
            done
            sleep 2
            rmmod mct || { echo "mct still in use (compositor holding the old device?)" >&2; exit 1; }
            modprobe mct
            echo "mct reloaded"
          '';
        in
        [
          reload
          # the old name
          (pkgs.writeShellScriptBin "mct-t2-reload" ''exec ${reload}/bin/mct-reload "$@"'')
        ];
      boot.kernelParams = lib.optional (cfg.consoleMap != null) "fbcon=map:${cfg.consoleMap}";
      # the USB aliases autoload mct when a supported device appears
      # raw device access for the logged-in user (tools/, after unbinding the driver).
      # uaccess must be tagged before 73-seat-late.rules, so not via extraRules (99-local).
      services.udev.packages = [
        (pkgs.writeTextDir "etc/udev/rules.d/70-mct.rules" ''
          SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="5209|520d|5600|5601", TAG+="uaccess"
        '')
      ];
    })

    (lib.mkIf (cfg.driver == "evdi") {
      boot.extraModulePackages = [ evdi ];
      boot.kernelModules = [ "evdi" ];
      boot.extraModprobeConfig = "options evdi initial_device_count=1";
      boot.blacklistedKernelModules = [ "mct" ];

      # video function of the tablet: accessible to the logged-in user, pulls in the daemon
      services.udev.extraRules = ''
        SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="5209|520d", TAG+="uaccess", TAG+="systemd", ENV{SYSTEMD_USER_WANTS}+="t2d.service"
      '';

      environment.systemPackages = [ t2d ];

      systemd.user.services.t2d = {
        description = "MCT Trigger II display daemon";
        wantedBy = [ "graphical-session.target" ];
        partOf = [ "graphical-session.target" ];
        after = [ "graphical-session.target" ];
        serviceConfig = {
          ExecStart = lib.getExe t2d;
          Restart = "always";
          RestartSec = 3;
        };
      };
    })
  ]);
}

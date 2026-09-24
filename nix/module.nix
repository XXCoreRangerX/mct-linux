# NixOS module: MCT Trigger II USB display (Wacom DTU-1031X) as a real monitor.
{ self }:
{ config, lib, pkgs, ... }:
let
  cfg = config.hardware.mct-t2;
  kernelPackages = config.boot.kernelPackages;
  kmod = pkgs.callPackage ./kmod.nix { inherit (kernelPackages) kernel; };
  evdi = pkgs.callPackage ./evdi.nix { inherit kernelPackages; };
  t2d = pkgs.callPackage ./t2d.nix { inherit evdi; };
in
{
  options.hardware.mct-t2 = {
    enable = lib.mkEnableOption "MCT Trigger II USB display support";
    driver = lib.mkOption {
      type = lib.types.enum [ "kernel" "evdi" ];
      default = "kernel";
      description = ''
        "kernel": native DRM driver mct_t2 (works on the text console too).
        "evdi": EVDI virtual connector + t2d userspace daemon.
      '';
    };
  };

  config = lib.mkIf cfg.enable (lib.mkMerge [
    (lib.mkIf (cfg.driver == "kernel") {
      boot.extraModulePackages = [ kmod ];
      # the USB alias autoloads mct_t2 when the tablet appears
    })

    (lib.mkIf (cfg.driver == "evdi") {
      boot.extraModulePackages = [ evdi ];
      boot.kernelModules = [ "evdi" ];
      boot.extraModprobeConfig = "options evdi initial_device_count=1";
      boot.blacklistedKernelModules = [ "mct_t2" ];

      # video function of the tablet: accessible to the logged-in user, pulls in the daemon
      services.udev.extraRules = ''
        SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="520d", TAG+="uaccess", TAG+="systemd", ENV{SYSTEMD_USER_WANTS}+="t2d.service"
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

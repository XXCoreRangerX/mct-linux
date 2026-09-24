# NixOS module: MCT Trigger II USB display (Wacom DTU-1031X) as a real monitor.
{ self }:
{ config, lib, pkgs, ... }:
let
  cfg = config.hardware.mct-t2;
  evdi = pkgs.callPackage ./evdi.nix { kernelPackages = config.boot.kernelPackages; };
  t2d = pkgs.callPackage ./t2d.nix { inherit evdi; };
in
{
  options.hardware.mct-t2 = {
    enable = lib.mkEnableOption "MCT Trigger II USB display support (EVDI + t2d daemon)";
    package = lib.mkOption {
      type = lib.types.package;
      default = t2d;
      description = "t2d package.";
    };
  };

  config = lib.mkIf cfg.enable {
    boot.extraModulePackages = [ evdi ];
    boot.kernelModules = [ "evdi" ];
    boot.extraModprobeConfig = "options evdi initial_device_count=1";

    # video function of the tablet: accessible to the logged-in user, pulls in the daemon
    services.udev.extraRules = ''
      SUBSYSTEM=="usb", ATTR{idVendor}=="0711", ATTR{idProduct}=="520d", TAG+="uaccess", TAG+="systemd", ENV{SYSTEMD_USER_WANTS}+="t2d.service"
    '';

    environment.systemPackages = [ cfg.package ];

    systemd.user.services.t2d = {
      description = "MCT Trigger II display daemon";
      wantedBy = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      after = [ "graphical-session.target" ];
      serviceConfig = {
        ExecStart = lib.getExe cfg.package;
        Restart = "always";
        RestartSec = 3;
      };
    };
  };
}

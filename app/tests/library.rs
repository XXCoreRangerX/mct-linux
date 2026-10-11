// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! The library against a fake sysfs tree: two T6 adapters and the T2.

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;

use mct::device::list_devices_in;
use mct::fake::{self, T2, T6_A, T6_B};
use mct::menu::{self, Item};
use mct::usb::{discover_in, select};
use mct::{switch, Chip, Error, Route, Snapshot, Warning};
use tempfile::TempDir;

fn tree() -> TempDir {
    let dir = TempDir::new().unwrap();
    fake::build(dir.path()).unwrap();
    dir
}

fn device(root: &Path, intf: &str) -> mct::Device {
    list_devices_in(root)
        .into_iter()
        .find(|d| d.interface == intf)
        .unwrap()
}

fn chmod(path: &Path, mode: u32) {
    fs::set_permissions(path, fs::Permissions::from_mode(mode)).unwrap();
}

/// True when permission bits don't stop us (running as root).
fn permissions_ignored(path: &Path) -> bool {
    fs::OpenOptions::new().write(true).open(path).is_ok()
}

#[test]
fn discovery() {
    let t = tree();
    let devs = list_devices_in(t.path());
    let names: Vec<&str> = devs.iter().map(|d| d.interface.as_str()).collect();
    assert_eq!(names, [T6_A, T2, T6_B]);

    let displays = discover_in(t.path());
    assert_eq!(displays.len(), 3);
    assert!(displays
        .iter()
        .all(|d| d.route == Route::Kernel && d.device.is_some()));
    assert_eq!(displays[1].chip, Chip::T2);
    assert_eq!(displays[0].usb_speed.as_deref(), Some("usb3"));
    assert_eq!(displays[2].usb_speed.as_deref(), Some("usb2"));
    assert_eq!(displays[0].product.as_deref(), Some("T6 USB Station"));

    assert!(discover_in(&t.path().join("nothing")).is_empty());
}

#[test]
fn reads() {
    let t = tree();
    let a = device(t.path(), T6_A).info().unwrap();
    assert_eq!(a.chip, Chip::T6);
    assert_eq!(a.model.as_deref(), Some("T6 USB Station"));
    assert_eq!(a.connector.as_deref(), Some("card2-HDMI-A-3"));
    assert_eq!(a.connector_short(), Some("HDMI-A-3"));
    assert_eq!(a.mode.as_deref(), Some("3440x1440@60"));
    assert_eq!(a.feature_level, Some(6));
    assert_eq!(a.vram_mb, Some(256));
    assert_eq!(a.bytes, Some(3_500_000_000));
    assert_eq!(a.format_summary(), "auto (RGB24)");
    assert_eq!(a.lossy, None);
    assert_eq!(a.brightness, None);

    let b = device(t.path(), T6_B).info().unwrap();
    assert_eq!(b.feature_level, Some(-1));
    assert_eq!(b.pixel_format.as_deref(), Some("-"));
    assert_eq!(b.format_summary(), "nv12");

    let d = device(t.path(), T2);
    let i = d.info().unwrap();
    assert_eq!(i.chip, Chip::T2);
    assert_eq!(i.backlight.as_deref(), Some(fake::T2_BACKLIGHT));
    assert_eq!((i.brightness, i.max_brightness), (Some(5), Some(9)));
    assert_eq!(i.lossy, Some(0));
    assert_eq!(i.format, None);
    assert_eq!(d.brightness().unwrap(), (5, 9));
    assert_eq!(d.get("lossy").unwrap(), "0");
    assert_eq!(d.get("max_brightness").unwrap(), "9");
    assert!(matches!(d.get("format"), Err(Error::Unsupported { .. })));
    assert!(matches!(d.get("nonsense"), Err(Error::UnknownKey(_))));

    // "-" means none for connector and backlight
    fake::set(t.path(), T2, "connector", "-").unwrap();
    fake::set(t.path(), T2, "backlight", "-").unwrap();
    let i = d.info().unwrap();
    assert_eq!(i.connector, None);
    assert_eq!(i.backlight, None);
    assert_eq!(i.brightness, None);
    assert!(matches!(
        d.set_brightness(1),
        Err(Error::Unsupported { .. })
    ));
}

#[test]
fn writes() {
    let t = tree();
    let a = device(t.path(), T6_A);
    let file =
        |intf: &str, k: &str| fs::read_to_string(fake::device_dir(t.path(), intf).join(k)).unwrap();

    a.set("format", "yuv444").unwrap();
    assert_eq!(file(T6_A, "format"), "yuv444\n");
    a.set("format", "nv12").unwrap();
    assert_eq!(file(T6_A, "format"), "nv12\n");
    assert!(matches!(a.set("format", "rgb16"), Err(Error::Invalid(_))));
    assert_eq!(file(T6_A, "format"), "nv12\n");

    a.set("all_modes", "on").unwrap();
    assert_eq!(file(T6_A, "all_modes"), "1\n");
    a.set("all_modes", "0").unwrap();
    assert_eq!(file(T6_A, "all_modes"), "0\n");
    assert!(matches!(
        a.set("all_modes", "maybe"),
        Err(Error::Invalid(_))
    ));

    assert!(matches!(
        a.set("lossy", "2"),
        Err(Error::Unsupported { .. })
    ));
    assert!(matches!(
        a.set("brightness", "2"),
        Err(Error::Unsupported { .. })
    ));
    assert!(matches!(
        a.set("mode", "640x480@60"),
        Err(Error::Invalid(_))
    ));
    assert!(matches!(a.set("colour", "red"), Err(Error::UnknownKey(_))));

    let d = device(t.path(), T2);
    d.set("lossy", "8").unwrap();
    assert_eq!(file(T2, "lossy"), "8\n");
    assert!(matches!(d.set("lossy", "9"), Err(Error::Invalid(_))));
    assert!(matches!(d.set("lossy", "-1"), Err(Error::Invalid(_))));
    assert!(matches!(
        d.set("format", "rgb24"),
        Err(Error::Unsupported { .. })
    ));

    d.set("brightness", "0").unwrap();
    assert_eq!(d.brightness().unwrap(), (0, 9));
    d.set_brightness(9).unwrap();
    assert_eq!(d.brightness().unwrap(), (9, 9));
    assert!(matches!(d.set("brightness", "10"), Err(Error::Invalid(_))));
    assert!(matches!(d.set("brightness", "x"), Err(Error::Invalid(_))));
    assert_eq!(Error::Invalid(String::new()).exit_code(), 2);
}

#[test]
fn permission_errors() {
    let t = tree();
    let a = device(t.path(), T6_A);
    let format = fake::device_dir(t.path(), T6_A).join("format");
    chmod(&format, 0o444);
    if permissions_ignored(&format) {
        eprintln!("running as root, skipping");
        return;
    }
    assert!(!a.can_write("format"));
    assert!(a.can_write("all_modes"));
    let e = a.set("format", "rgb24").unwrap_err();
    assert!(e.is_permission());
    assert_eq!(e.exit_code(), 3);
    assert!(e.to_string().contains("group \"video\""), "{e}");
    assert!(e.to_string().contains("udev"), "{e}");
    // validation comes first
    assert!(matches!(a.set("format", "bad"), Err(Error::Invalid(_))));

    let d = device(t.path(), T2);
    assert!(d.can_write("brightness"));
    let b = fake::backlight_dir(t.path(), fake::T2_BACKLIGHT).join("brightness");
    chmod(&b, 0o444);
    assert!(!d.can_write("brightness"));
    assert!(d.set_brightness(1).unwrap_err().is_permission());
}

#[test]
fn hotplug_and_gone() {
    let t = tree();
    let a = device(t.path(), T6_A);
    fake::remove_device(t.path(), T6_A).unwrap();
    assert!(matches!(a.info(), Err(Error::Gone(_))));
    assert!(matches!(a.set_format("auto"), Err(Error::Gone(_))));
    assert_eq!(discover_in(t.path()).len(), 2);
    fake::add_t6_a(t.path()).unwrap();
    assert_eq!(discover_in(t.path()).len(), 3);
    assert!(a.info().is_ok());
}

#[test]
fn routes() {
    let t = tree();
    let route = |intf: &str| {
        discover_in(t.path())
            .into_iter()
            .find(|d| d.interface == intf)
            .unwrap()
            .route
    };

    fake::unbind(t.path(), T2).unwrap();
    assert_eq!(route(T2), Route::NotBound);
    assert!(discover_in(t.path())
        .iter()
        .find(|d| d.interface == T2)
        .unwrap()
        .device
        .is_none());

    fake::remove_module(t.path(), "mct").unwrap();
    assert_eq!(route(T2), Route::NotLoaded);
    assert!(Route::NotLoaded.hint().unwrap().contains("modprobe mct"));

    // the legacy daemon needs both the process and evdi
    fake::add_process(t.path(), 4242, "wacom-t2d").unwrap();
    assert_eq!(route(T2), Route::NotLoaded);
    fake::add_module(t.path(), "evdi").unwrap();
    fake::bind_other(t.path(), T2, "usbfs").unwrap();
    assert_eq!(route(T2), Route::Legacy);
    assert_eq!(Route::Legacy.label(), "Legacy: wacom-t2d over EVDI");

    // only the tablet has a legacy route
    fake::unbind(t.path(), T6_B).unwrap();
    fake::bind_other(t.path(), T6_B, "usbfs").unwrap();
    assert_eq!(route(T6_B), Route::Other("usbfs".into()));

    fake::add_module(t.path(), "mct").unwrap();
    fake::bind(t.path(), T2, fake::T2_FILES).unwrap();
    assert_eq!(route(T2), Route::Kernel);
}

#[test]
fn legacy_availability() {
    let t = tree();
    let displays = discover_in(t.path());
    let t6 = displays.iter().find(|d| d.chip == Chip::T6).unwrap();
    let t2 = displays.iter().find(|d| d.chip == Chip::T2).unwrap();
    assert!(switch::legacy_unavailable_in(t.path(), t6).is_some());
    assert_eq!(
        switch::legacy_unavailable_in(t.path(), t2),
        Some("the evdi module is not installed")
    );
    fake::add_module(t.path(), "evdi").unwrap();
    assert_eq!(switch::legacy_unavailable_in(t.path(), t2), None);
}

#[test]
fn selection() {
    let t = tree();
    let displays = discover_in(t.path());
    let pick = |s: Option<&str>| select(&displays, s).map(|d| d.interface.clone());
    assert_eq!(pick(Some(T6_B)).unwrap(), T6_B);
    assert_eq!(pick(Some("HDMI-A-3")).unwrap(), T6_A);
    assert_eq!(pick(Some("card2-HDMI-A-3")).unwrap(), T6_A);
    assert_eq!(pick(Some("USB-1")).unwrap(), T2);
    assert_eq!(pick(Some("2")).unwrap(), T6_B);
    assert!(matches!(pick(Some("7")), Err(Error::NotFound(_))));
    assert!(matches!(pick(Some("DP-1")), Err(Error::NotFound(_))));
    assert!(matches!(pick(None), Err(Error::NeedDevice(3))));

    fake::set(t.path(), T6_B, "connector", "card2-HDMI-A-3").unwrap();
    let displays = discover_in(t.path());
    assert!(matches!(
        select(&displays, Some("HDMI-A-3")),
        Err(Error::Ambiguous(..))
    ));

    fake::remove_device(t.path(), T6_A).unwrap();
    fake::remove_device(t.path(), T6_B).unwrap();
    let displays = discover_in(t.path());
    assert_eq!(select(&displays, None).unwrap().interface, T2);
    assert!(matches!(select(&[], None), Err(Error::NoDevices)));
}

#[test]
fn warnings() {
    let t = tree();
    assert!(device(t.path(), T6_A).info().unwrap().warnings().is_empty());
    let w = device(t.path(), T6_B).info().unwrap().warnings();
    assert_eq!(
        w,
        [
            Warning::Usb2,
            Warning::TransferErrors {
                errors: 2,
                recoveries: 1
            }
        ]
    );
    assert!(w[0].text().contains("USB 2"));
    assert!(w[0].is_serious() && !w[1].is_serious());
    assert_eq!(
        w[1].text(),
        "Transfer errors since connect: 2, device restarts: 1"
    );
    // the tablet runs on USB 2 by design
    assert!(device(t.path(), T2).info().unwrap().warnings().is_empty());
}

#[test]
fn tray_menu() {
    let t = tree();
    let snaps: Vec<Snapshot> = discover_in(t.path())
        .into_iter()
        .map(Snapshot::take)
        .collect();
    let items = menu::items(&snaps, &[None, Some("Failed: x".into()), None]);
    let text: Vec<String> = items
        .iter()
        .filter_map(|i| match i {
            Item::Header(s) | Item::Note(s) => Some(s.clone()),
            _ => None,
        })
        .collect();
    assert_eq!(text[0], "T6 USB Station: HDMI-A-3");
    assert!(text.contains(&"Mode: 3440x1440@60".to_string()));
    assert!(text.contains(&"Format: auto (RGB24)".to_string()));
    assert!(text.contains(&"DTU-1031X: USB-1".to_string()));
    assert!(text.contains(&"Brightness: 5 of 9".to_string()));
    assert!(text.contains(&"USB 2 link".to_string()));
    assert!(text.contains(&"Transfer errors: 2".to_string()));
    assert!(text.contains(&"Failed: x".to_string()));
    assert!(items.contains(&Item::Formats {
        device: 0,
        selected: Some(0)
    }));
    assert!(items.contains(&Item::Formats {
        device: 2,
        selected: Some(4)
    }));
    assert!(items.contains(&Item::AllModes {
        device: 2,
        on: true
    }));
    assert!(items.contains(&Item::Lossy {
        device: 1,
        selected: Some(0)
    }));
    assert!(items.iter().any(|i| matches!(
        i,
        Item::Brightness {
            device: 1,
            selected: Some(5),
            ..
        }
    )));
    assert_eq!(items.iter().filter(|i| **i == Item::Separator).count(), 2);

    // a display without the kernel driver shows its state only
    fake::unbind(t.path(), T2).unwrap();
    let snaps: Vec<Snapshot> = discover_in(t.path())
        .into_iter()
        .map(Snapshot::take)
        .collect();
    let items = menu::items(&snaps, &[]);
    assert!(items.contains(&Item::Header("DTU-1031X: 3-1:1.0".into())));
    assert!(items.contains(&Item::Note("Not bound".into())));
    assert!(!items
        .iter()
        .any(|i| matches!(i, Item::Lossy { device: 1, .. })));

    assert_eq!(menu::items(&[], &[]), vec![Item::Empty]);
}

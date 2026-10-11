// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Every mctctl command against a fake sysfs tree.

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::process::{Command, Output};
use std::thread::sleep;
use std::time::Duration;

use mct::fake::{self, T2, T6_A, T6_B};
use tempfile::TempDir;

fn tree() -> TempDir {
    let dir = TempDir::new().unwrap();
    fake::build(dir.path()).unwrap();
    dir
}

fn mctctl(root: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_mctctl"))
        .args(args)
        .env("MCT_SYSFS_ROOT", root)
        .output()
        .unwrap()
}

fn stdout(o: &Output) -> String {
    String::from_utf8_lossy(&o.stdout).into_owned()
}

fn stderr(o: &Output) -> String {
    String::from_utf8_lossy(&o.stderr).into_owned()
}

/// Runs mctctl, expects success, returns stdout.
fn ok(root: &Path, args: &[&str]) -> String {
    let o = mctctl(root, args);
    assert!(o.status.success(), "mctctl {args:?}: {}", stderr(&o));
    stdout(&o)
}

/// Runs mctctl, expects this exit status, returns stderr.
fn fails(root: &Path, args: &[&str], code: i32) -> String {
    let o = mctctl(root, args);
    assert_eq!(
        o.status.code(),
        Some(code),
        "mctctl {args:?}: {}{}",
        stdout(&o),
        stderr(&o)
    );
    stderr(&o)
}

fn file(root: &Path, intf: &str, key: &str) -> String {
    fs::read_to_string(fake::device_dir(root, intf).join(key)).unwrap()
}

#[test]
fn list() {
    let t = tree();
    let out = ok(t.path(), &["list"]);
    let lines: Vec<&str> = out.lines().collect();
    assert!(
        lines[0].starts_with("#") && lines[0].contains("DRIVER"),
        "{out}"
    );
    assert!(
        lines[1].contains(T6_A)
            && lines[1].contains("HDMI-A-3")
            && lines[1].contains("auto (RGB24)")
    );
    assert!(lines[2].contains(T2) && lines[2].contains("USB-1") && lines[2].contains("lossy 0"));
    assert!(lines[3].starts_with("2!") && lines[3].contains("off") && lines[3].contains("nv12"));
    assert!(lines[1].trim_end().ends_with("mct"));
    assert!(out.contains("2! HDMI-A-4: Connected over USB 2"), "{out}");
    assert!(out.contains("Transfer errors since connect: 2"), "{out}");

    let v: serde_json::Value = serde_json::from_str(&ok(t.path(), &["list", "--json"])).unwrap();
    let a = v.as_array().unwrap();
    assert_eq!(a.len(), 3);
    assert_eq!(a[0]["index"], 0);
    assert_eq!(a[0]["interface"], T6_A);
    assert_eq!(a[0]["driver"], "mct");
    assert_eq!(a[0]["bytes"], 3_500_000_000u64);
    assert_eq!(a[1]["brightness"], 5);
    assert_eq!(a[2]["warnings"].as_array().unwrap().len(), 2);
}

#[test]
fn list_unbound_and_empty() {
    let t = tree();
    fake::unbind(t.path(), T2).unwrap();
    let out = ok(t.path(), &["list"]);
    assert!(out.lines().nth(2).unwrap().contains("not-bound"), "{out}");
    assert!(out.contains("Not bound. Replug the device"), "{out}");
    let v: serde_json::Value =
        serde_json::from_str(&ok(t.path(), &["info", T2, "--json"])).unwrap();
    assert_eq!(v["driver"], "not-bound");
    assert_eq!(v["model"], "DTU-1031X");
    assert!(v.get("lossy").is_none());
    assert!(fails(t.path(), &["get", T2, "lossy"], 1).contains("not using the kernel driver"));
    assert!(ok(t.path(), &["info", T2]).contains("driver:          Not bound"));

    let empty = TempDir::new().unwrap();
    let o = mctctl(empty.path(), &["list"]);
    assert!(o.status.success());
    assert_eq!(stdout(&o), "");
    assert!(stderr(&o).contains("no MCT displays found"));
    assert_eq!(ok(empty.path(), &["list", "--json"]).trim(), "[]");
    assert!(fails(empty.path(), &["info"], 1).contains("no mct devices found"));
}

#[test]
fn info_and_selection() {
    let t = tree();
    let by_intf = ok(t.path(), &["info", T6_A]);
    assert!(by_intf.contains("format:          auto"));
    assert!(by_intf.contains("driver:          Kernel driver (mct)"));
    assert_eq!(ok(t.path(), &["info", "HDMI-A-3"]), by_intf);
    assert_eq!(ok(t.path(), &["info", "card2-HDMI-A-3"]), by_intf);
    assert_eq!(ok(t.path(), &["info", "0"]), by_intf);
    assert!(ok(t.path(), &["info", T6_B]).contains("warning: Connected over USB 2"));

    let v: serde_json::Value =
        serde_json::from_str(&ok(t.path(), &["info", "USB-1", "--json"])).unwrap();
    assert_eq!(v["chip"], "t2");
    assert_eq!(v["max_brightness"], 9);
    assert_eq!(v["backlight"], fake::T2_BACKLIGHT);

    assert!(fails(t.path(), &["info"], 2).contains("3 devices found"));
    assert!(fails(t.path(), &["info", "DP-9"], 1).contains("no device matches"));

    // one device: DEV may be left out
    fake::remove_device(t.path(), T6_A).unwrap();
    fake::remove_device(t.path(), T6_B).unwrap();
    assert_eq!(ok(t.path(), &["get", "lossy"]), "0\n");
    assert!(ok(t.path(), &["info"]).contains(T2));
}

#[test]
fn get() {
    let t = tree();
    assert_eq!(ok(t.path(), &["get", T6_A, "mode"]), "3440x1440@60\n");
    assert_eq!(ok(t.path(), &["get", "HDMI-A-4", "pixel_format"]), "-\n");
    assert_eq!(ok(t.path(), &["get", "1", "brightness"]), "5\n");
    assert_eq!(ok(t.path(), &["get", "USB-1", "backlight"]), "mct_t2-1\n");
    assert!(fails(t.path(), &["get", "USB-1", "format"], 2).contains("not available"));
    assert!(fails(t.path(), &["get", "USB-1", "colour"], 2).contains("unknown key"));
    assert!(fails(t.path(), &["get", "mode"], 2).contains("devices found"));
    fails(t.path(), &["get"], 2);
}

#[test]
fn set() {
    let t = tree();
    ok(t.path(), &["set", "HDMI-A-3", "format", "rgb32"]);
    assert_eq!(file(t.path(), T6_A, "format"), "rgb32\n");
    ok(t.path(), &["set", T6_A, "all_modes", "on"]);
    assert_eq!(file(t.path(), T6_A, "all_modes"), "1\n");
    ok(t.path(), &["set", "USB-1", "lossy", "4"]);
    assert_eq!(file(t.path(), T2, "lossy"), "4\n");
    ok(t.path(), &["set", "USB-1", "brightness", "0"]);
    assert_eq!(ok(t.path(), &["get", "USB-1", "brightness"]), "0\n");

    assert!(fails(t.path(), &["set", T6_A, "format", "rgb16"], 2).contains("invalid format"));
    assert!(fails(t.path(), &["set", T6_A, "all_modes", "2"], 2).contains("all_modes"));
    assert!(fails(t.path(), &["set", "USB-1", "lossy", "9"], 2).contains("0 to 8"));
    assert!(fails(t.path(), &["set", "USB-1", "brightness", "10"], 2).contains("0 to 9"));
    assert!(fails(t.path(), &["set", "USB-1", "format", "auto"], 2).contains("not available"));
    assert!(fails(t.path(), &["set", T6_A, "mode", "off"], 2).contains("read-only"));
    assert!(fails(t.path(), &["set", T6_A, "colour", "red"], 2).contains("unknown key"));
    assert_eq!(file(t.path(), T6_A, "format"), "rgb32\n");
}

#[test]
fn set_without_permission() {
    let t = tree();
    let path = fake::device_dir(t.path(), T6_A).join("format");
    fs::set_permissions(&path, fs::Permissions::from_mode(0o444)).unwrap();
    if fs::OpenOptions::new().write(true).open(&path).is_ok() {
        eprintln!("running as root, skipping");
        return;
    }
    let err = fails(t.path(), &["set", T6_A, "format", "nv12"], 3);
    assert!(err.contains("permission denied"), "{err}");
    assert!(err.contains("group \"video\""), "{err}");
    assert!(err.contains("udev"), "{err}");
    assert_eq!(file(t.path(), T6_A, "format"), "auto\n");

    let b = fake::backlight_dir(t.path(), fake::T2_BACKLIGHT).join("brightness");
    fs::set_permissions(&b, fs::Permissions::from_mode(0o444)).unwrap();
    assert!(fails(t.path(), &["set", "USB-1", "brightness", "3"], 3).contains("group \"video\""));
}

#[test]
fn stats() {
    let t = tree();
    let out = ok(t.path(), &["stats", T6_A, "--interval", "0.1"]);
    assert_eq!(out.lines().count(), 1);
    assert!(
        out.starts_with("HDMI-A-3: 0.0 updates/s, 0.00 MB/s, errors 0, recoveries 0"),
        "{out}"
    );
    assert!(out.contains("3.5 GB sent"));
    let out = ok(t.path(), &["stats", "USB-1", "--interval", "0.1"]);
    assert!(!out.contains("recoveries"), "{out}");

    // counters move while watching: some line shows a rate
    let root = t.path().to_path_buf();
    let bump = std::thread::spawn(move || {
        for n in 1..=4u64 {
            sleep(Duration::from_millis(200));
            fake::set(&root, T6_A, "updates", &(1200 + 30 * n).to_string()).unwrap();
            fake::set(
                &root,
                T6_A,
                "bytes",
                &(3_500_000_000 + 2_000_000 * n).to_string(),
            )
            .unwrap();
        }
    });
    let out = ok(
        t.path(),
        &[
            "stats",
            T6_A,
            "--watch",
            "--count",
            "3",
            "--interval",
            "0.4",
            "--json",
        ],
    );
    bump.join().unwrap();
    let lines: Vec<serde_json::Value> = out
        .lines()
        .map(|l| serde_json::from_str(l).unwrap())
        .collect();
    assert_eq!(lines.len(), 3);
    assert!(
        lines
            .iter()
            .any(|l| l["updates_per_s"].as_f64().unwrap() > 0.0),
        "{out}"
    );
    assert!(
        lines.iter().any(|l| l["mb_per_s"].as_f64().unwrap() > 0.0),
        "{out}"
    );
    assert_eq!(lines[0]["connector"], "HDMI-A-3");

    fails(t.path(), &["stats", T6_A, "--interval", "0"], 2);
    fake::unbind(t.path(), T2).unwrap();
    fails(t.path(), &["stats", T2], 1);
}

#[test]
fn driver() {
    let t = tree();
    assert!(ok(t.path(), &["driver", "USB-1", "kernel"]).contains("already using Kernel driver"));
    assert!(fails(t.path(), &["driver", "HDMI-A-3", "legacy"], 1).contains("DTU-1031X only"));
    assert!(fails(t.path(), &["driver", "USB-1", "legacy"], 1).contains("evdi"));
    assert!(fails(t.path(), &["driver", "USB-1", "userspace"], 2).contains("kernel or legacy"));

    // with evdi present the switch gets as far as the helper, which refuses
    // to touch a fake tree
    fake::add_module(t.path(), "evdi").unwrap();
    assert!(fails(t.path(), &["driver", "USB-1", "legacy"], 1).contains("MCT_SYSFS_ROOT"));

    fake::unbind(t.path(), T2).unwrap();
    fake::add_process(t.path(), 99, "wacom-t2d").unwrap();
    fake::bind_other(t.path(), T2, "usbfs").unwrap();
    assert!(ok(t.path(), &["driver", T2, "legacy"]).contains("already using Legacy"));
    assert!(ok(t.path(), &["list"])
        .lines()
        .nth(2)
        .unwrap()
        .contains("legacy"));
    // switching back would stop the daemon: refused on a fake tree too
    assert!(fails(t.path(), &["driver", T2, "kernel"], 1).contains("MCT_SYSFS_ROOT"));
}

#[test]
fn settings_saved_and_applied() {
    let t = tree();
    let saved = t.path().join("settings.toml");
    ok(t.path(), &["set", "HDMI-A-3", "format", "nv12"]);
    ok(t.path(), &["set", "USB-1", "lossy", "3"]);
    let text = fs::read_to_string(&saved).unwrap();
    assert!(text.contains("format = \"nv12\""), "{text}");
    assert!(text.contains("0711:5600/port-2-2"), "{text}");
    assert!(ok(t.path(), &["saved"]).contains("format=nv12"));

    // a temporary change isn't saved
    ok(t.path(), &["set", "--temporary", "HDMI-A-3", "all_modes", "1"]);
    assert!(!fs::read_to_string(&saved).unwrap().contains("all_modes = true"));

    // replug: the driver starts with its defaults, apply restores
    fake::unbind(t.path(), T6_A).unwrap();
    fake::bind(t.path(), T6_A, fake::T6_A_FILES).unwrap();
    assert_eq!(file(t.path(), T6_A, "format").trim(), "auto");
    assert!(ok(t.path(), &["apply", "HDMI-A-3"]).contains("applied format"));
    assert_eq!(file(t.path(), T6_A, "format").trim(), "nv12");
    assert!(ok(t.path(), &["apply", "HDMI-A-3"]).contains("already as saved"));

    assert!(ok(t.path(), &["forget", "HDMI-A-3"]).contains("forgotten"));
    assert!(ok(t.path(), &["apply", "HDMI-A-3"]).contains("no saved settings"));
    // the tablet's entry stays
    assert!(ok(t.path(), &["saved"]).contains("lossy=3"));
}

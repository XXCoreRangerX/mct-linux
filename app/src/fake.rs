// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! A fake sysfs tree for tests and for trying the app without a device.
//! Point `MCT_SYSFS_ROOT` at the directory given to `build`.

use std::fs;
use std::io;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

pub const T6_A: &str = "2-2:1.0";
pub const T6_B: &str = "4-1:1.0";
pub const T2: &str = "3-1:1.0";
pub const T2_BACKLIGHT: &str = "mct_t2-1";

const RW: &[&str] = &["format", "all_modes", "lossy"];

fn put(dir: &Path, name: &str, value: &str) -> io::Result<()> {
    let path = dir.join(name);
    fs::write(&path, format!("{value}\n"))?;
    let mode = if RW.contains(&name) || name == "brightness" {
        0o664
    } else {
        0o444
    };
    fs::set_permissions(&path, fs::Permissions::from_mode(mode))
}

pub fn device_dir(root: &Path, intf: &str) -> PathBuf {
    root.join("sys/bus/usb/drivers/mct").join(intf).join("mct")
}

pub fn backlight_dir(root: &Path, name: &str) -> PathBuf {
    root.join("sys/class/backlight").join(name)
}

fn usb_dir(root: &Path, dev: &str) -> PathBuf {
    root.join("sys/devices/usb").join(dev)
}

fn symlink(target: &Path, link: &Path) -> io::Result<()> {
    if let Some(parent) = link.parent() {
        fs::create_dir_all(parent)?;
    }
    if link.symlink_metadata().is_ok() {
        fs::remove_file(link)?;
    }
    std::os::unix::fs::symlink(target, link)
}

/// Plugs in a USB device with its interface 0 (`<dev>:1.0`), no driver bound.
/// `speed` is in Mbit/s as in sysfs (480, 5000).
pub fn add_usb(
    root: &Path,
    dev: &str,
    product_id: &str,
    product: &str,
    speed: &str,
) -> io::Result<String> {
    let dir = usb_dir(root, dev);
    let intf = format!("{dev}:1.0");
    fs::create_dir_all(dir.join(&intf))?;
    for (k, v) in [
        ("idVendor", "0711"),
        ("idProduct", product_id),
        ("product", product),
        ("speed", speed),
    ] {
        put(&dir, k, v)?;
    }
    let devices = root.join("sys/bus/usb/devices");
    symlink(&dir, &devices.join(dev))?;
    symlink(&dir.join(&intf), &devices.join(&intf))?;
    Ok(intf)
}

/// Binds an interface to mct: the driver links and the mct directory with
/// the given files.
pub fn bind(root: &Path, intf: &str, files: &[(&str, &str)]) -> io::Result<PathBuf> {
    let drv = root.join("sys/bus/usb/drivers/mct");
    fs::create_dir_all(&drv)?;
    // the driver directory also holds these; discovery must skip them
    for f in ["bind", "unbind", "uevent", "new_id"] {
        if !drv.join(f).exists() {
            fs::write(drv.join(f), "")?;
        }
    }
    fs::create_dir_all(drv.join("module"))?;
    let dev = intf.split(':').next().unwrap_or(intf);
    let intf_dir = usb_dir(root, dev).join(intf);
    fs::create_dir_all(&intf_dir)?;
    symlink(&intf_dir, &drv.join(intf))?;
    symlink(&drv, &intf_dir.join("driver"))?;
    let dir = intf_dir.join("mct");
    fs::create_dir_all(&dir)?;
    for (k, v) in files {
        put(&dir, k, v)?;
    }
    Ok(dir)
}

/// Unbinds an interface from mct: its mct directory and links disappear.
pub fn unbind(root: &Path, intf: &str) -> io::Result<()> {
    let dev = intf.split(':').next().unwrap_or(intf);
    let intf_dir = usb_dir(root, dev).join(intf);
    let _ = fs::remove_file(root.join("sys/bus/usb/drivers/mct").join(intf));
    let _ = fs::remove_file(intf_dir.join("driver"));
    match fs::remove_dir_all(intf_dir.join("mct")) {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(e),
        _ => Ok(()),
    }
}

/// Binds an interface to another driver (for example usbfs).
pub fn bind_other(root: &Path, intf: &str, driver: &str) -> io::Result<()> {
    let dev = intf.split(':').next().unwrap_or(intf);
    let drv = root.join("sys/bus/usb/drivers").join(driver);
    fs::create_dir_all(&drv)?;
    symlink(&drv, &usb_dir(root, dev).join(intf).join("driver"))
}

/// Unplugs a device.
pub fn remove_device(root: &Path, intf: &str) -> io::Result<()> {
    unbind(root, intf)?;
    let dev = intf.split(':').next().unwrap_or(intf);
    let devices = root.join("sys/bus/usb/devices");
    let _ = fs::remove_file(devices.join(dev));
    let _ = fs::remove_file(devices.join(intf));
    fs::remove_dir_all(usb_dir(root, dev))
}

pub fn add_module(root: &Path, name: &str) -> io::Result<()> {
    fs::create_dir_all(root.join("sys/module").join(name))
}

pub fn remove_module(root: &Path, name: &str) -> io::Result<()> {
    fs::remove_dir_all(root.join("sys/module").join(name))
}

/// A running process, as seen in /proc.
pub fn add_process(root: &Path, pid: u32, comm: &str) -> io::Result<()> {
    let dir = root.join("proc").join(pid.to_string());
    fs::create_dir_all(&dir)?;
    fs::write(dir.join("comm"), format!("{comm}\n"))
}

pub fn add_backlight(root: &Path, name: &str, brightness: u32, max: u32) -> io::Result<PathBuf> {
    let dir = backlight_dir(root, name);
    fs::create_dir_all(&dir)?;
    put(&dir, "brightness", &brightness.to_string())?;
    put(&dir, "max_brightness", &max.to_string())?;
    Ok(dir)
}

/// Overwrites one file of a bound device, for counters and the like.
pub fn set(root: &Path, intf: &str, key: &str, value: &str) -> io::Result<()> {
    let path = device_dir(root, intf).join(key);
    let mode = fs::metadata(&path)
        .map(|m| m.permissions().mode())
        .unwrap_or(0o444);
    fs::set_permissions(&path, fs::Permissions::from_mode(0o644))?;
    fs::write(&path, format!("{value}\n"))?;
    fs::set_permissions(&path, fs::Permissions::from_mode(mode))
}

pub const T6_A_FILES: &[(&str, &str)] = &[
    ("chip", "t6"),
    ("model", "T6 USB Station"),
    ("connector", "card2-HDMI-A-3"),
    ("mode", "3440x1440@60"),
    ("pixel_format", "RGB24"),
    ("feature_level", "6"),
    ("vram_mb", "256"),
    ("usb_speed", "usb3"),
    ("updates", "1200"),
    ("bytes", "3500000000"),
    ("errors", "0"),
    ("recoveries", "0"),
    ("format", "auto"),
    ("all_modes", "0"),
];

pub const T6_B_FILES: &[(&str, &str)] = &[
    ("chip", "t6"),
    ("model", "T6 USB Station"),
    ("connector", "card3-HDMI-A-4"),
    ("mode", "off"),
    ("pixel_format", "-"),
    ("feature_level", "-1"),
    ("vram_mb", "256"),
    ("usb_speed", "usb2"),
    ("updates", "0"),
    ("bytes", "0"),
    ("errors", "2"),
    ("recoveries", "1"),
    ("format", "nv12"),
    ("all_modes", "1"),
];

pub const T2_FILES: &[(&str, &str)] = &[
    ("chip", "t2"),
    ("model", "DTU-1031X"),
    ("connector", "card1-USB-1"),
    ("mode", "1024x600@60"),
    ("updates", "500"),
    ("bytes", "120000000"),
    ("errors", "0"),
    ("lossy", "0"),
    ("backlight", T2_BACKLIGHT),
];

pub fn add_t6_a(root: &Path) -> io::Result<PathBuf> {
    add_usb(root, "2-2", "5600", "T6 USB Station", "5000")?;
    bind(root, T6_A, T6_A_FILES)
}

pub fn add_t6_b(root: &Path) -> io::Result<PathBuf> {
    add_usb(root, "4-1", "5600", "T6 USB Station", "480")?;
    bind(root, T6_B, T6_B_FILES)
}

pub fn add_t2(root: &Path) -> io::Result<PathBuf> {
    add_usb(root, "3-1", "520d", "DTU-1031X", "480")?;
    add_backlight(root, T2_BACKLIGHT, 5, 9)?;
    bind(root, T2, T2_FILES)
}

/// Two T6 adapters and the T2 tablet with its backlight.
pub fn build(root: &Path) -> io::Result<()> {
    add_module(root, "mct")?;
    add_t6_a(root)?;
    add_t6_b(root)?;
    add_t2(root)?;
    Ok(())
}

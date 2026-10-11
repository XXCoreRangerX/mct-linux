// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Device discovery, reads and validated writes.
//!
//! Per bound device the driver has a directory
//! `/sys/bus/usb/drivers/mct/<interface>/mct/` with one value per file. A T2's
//! brightness is its backlight device in `/sys/class/backlight/`. The root
//! (`/` by default) can be moved with `MCT_SYSFS_ROOT` for tests.

use std::cmp::Ordering;
use std::fmt;
use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use serde::Serialize;

pub const FORMATS: [&str; 5] = ["auto", "rgb24", "rgb32", "yuv444", "nv12"];
pub const LOSSY_MAX: u32 = 8;

/// Keys `get` understands, in the order `info` prints them.
pub const KEYS: &[&str] = &[
    "interface",
    "chip",
    "model",
    "connector",
    "mode",
    "pixel_format",
    "feature_level",
    "vram_mb",
    "usb_speed",
    "updates",
    "bytes",
    "errors",
    "recoveries",
    "format",
    "all_modes",
    "lossy",
    "backlight",
    "brightness",
    "max_brightness",
];

/// Keys `set` accepts.
pub const SETTABLE: &[&str] = &["format", "all_modes", "lossy", "brightness"];

pub const PERMISSION_HINT: &str = "Add your user to group \"video\" (then log in again) \
     and make sure the mct udev rules are installed (see the README).";

const ENODEV: i32 = 19;
const EINVAL: i32 = 22;

/// The directory `sys/` lives in: `$MCT_SYSFS_ROOT`, or `/`.
pub fn sysfs_root() -> PathBuf {
    match std::env::var_os("MCT_SYSFS_ROOT") {
        Some(v) if !v.is_empty() => PathBuf::from(v),
        _ => PathBuf::from("/"),
    }
}

fn driver_dir(root: &Path) -> PathBuf {
    root.join("sys/bus/usb/drivers/mct")
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Chip {
    T2,
    T6,
    #[default]
    Unknown,
}

impl Chip {
    fn parse(s: &str) -> Chip {
        match s {
            "t2" => Chip::T2,
            "t6" => Chip::T6,
            _ => Chip::Unknown,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Chip::T2 => "t2",
            Chip::T6 => "t6",
            Chip::Unknown => "unknown",
        }
    }
}

impl fmt::Display for Chip {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str(self.name())
    }
}

#[derive(Debug)]
pub enum Error {
    /// No device is bound to mct.
    NoDevices,
    /// The device was not given and there are several.
    NeedDevice(usize),
    NotFound(String),
    Ambiguous(String, Vec<String>),
    /// The device went away (unplugged or unbound).
    Gone(String),
    UnknownKey(String),
    /// The key exists but not on this chip (or this device lacks the file).
    Unsupported {
        key: String,
        chip: Chip,
    },
    Invalid(String),
    Permission {
        path: PathBuf,
        hint: &'static str,
    },
    /// The display is not bound to mct, so it has no settings.
    NotKernel {
        interface: String,
        route: String,
    },
    /// Switching between the kernel driver and the legacy daemon failed.
    Switch(String),
    Io {
        path: PathBuf,
        err: io::Error,
    },
}

impl Error {
    /// Exit status for mctctl: 2 for bad input, 3 for permission problems,
    /// 1 for everything else.
    pub fn exit_code(&self) -> i32 {
        match self {
            Error::Permission { .. } => 3,
            Error::Invalid(_)
            | Error::Unsupported { .. }
            | Error::UnknownKey(_)
            | Error::NeedDevice(_)
            | Error::Ambiguous(..) => 2,
            _ => 1,
        }
    }

    pub fn is_permission(&self) -> bool {
        matches!(self, Error::Permission { .. })
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::NoDevices => write!(f, "no mct devices found"),
            Error::NeedDevice(n) => {
                write!(f, "{n} devices found, name one (interface, connector or index from \"mctctl list\")")
            }
            Error::NotFound(s) => write!(f, "no device matches \"{s}\""),
            Error::Ambiguous(s, all) => {
                write!(f, "\"{s}\" matches several devices: {}", all.join(", "))
            }
            Error::Gone(intf) => write!(f, "device {intf} is gone"),
            Error::UnknownKey(k) => {
                write!(f, "unknown key \"{k}\" (known: {})", KEYS.join(", "))
            }
            Error::Unsupported { key, chip } => {
                write!(f, "{key} is not available on this device ({chip})")
            }
            Error::Invalid(msg) => f.write_str(msg),
            Error::Permission { path, hint } => {
                write!(f, "permission denied writing {}. {hint}", path.display())
            }
            Error::NotKernel { interface, route } => {
                write!(f, "{interface} is not using the kernel driver ({route})")
            }
            Error::Switch(msg) => write!(f, "cannot switch the driver: {msg}"),
            Error::Io { path, err } => write!(f, "{}: {err}", path.display()),
        }
    }
}

impl std::error::Error for Error {}

/// Everything known about a device at one moment.
#[derive(Clone, Debug, Default, PartialEq, Serialize)]
pub struct Info {
    pub interface: String,
    pub chip: Chip,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub connector: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pixel_format: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub feature_level: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub vram_mb: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub usb_speed: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub updates: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub bytes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub errors: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub recoveries: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub format: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub all_modes: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub lossy: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub backlight: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub brightness: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub max_brightness: Option<u32>,
}

impl Info {
    /// A key's value as text, as `get` prints it.
    pub fn value(&self, key: &str) -> Option<String> {
        fn s<T: ToString>(v: &Option<T>) -> Option<String> {
            v.as_ref().map(|v| v.to_string())
        }
        match key {
            "interface" => Some(self.interface.clone()),
            "chip" => Some(self.chip.to_string()),
            "model" => s(&self.model),
            "connector" => s(&self.connector),
            "mode" => s(&self.mode),
            "pixel_format" => s(&self.pixel_format),
            "feature_level" => s(&self.feature_level),
            "vram_mb" => s(&self.vram_mb),
            "usb_speed" => s(&self.usb_speed),
            "updates" => s(&self.updates),
            "bytes" => s(&self.bytes),
            "errors" => s(&self.errors),
            "recoveries" => s(&self.recoveries),
            "format" => s(&self.format),
            "all_modes" => s(&self.all_modes),
            "lossy" => s(&self.lossy),
            "backlight" => s(&self.backlight),
            "brightness" => s(&self.brightness),
            "max_brightness" => s(&self.max_brightness),
            _ => None,
        }
    }

    /// All known keys with their values, in `KEYS` order.
    pub fn entries(&self) -> Vec<(&'static str, String)> {
        KEYS.iter()
            .filter_map(|k| self.value(k).map(|v| (*k, v)))
            .collect()
    }

    /// The connector as the compositor names it (`HDMI-A-3` for `card2-HDMI-A-3`).
    pub fn connector_short(&self) -> Option<&str> {
        self.connector.as_deref().map(short_connector)
    }

    pub fn name(&self) -> &str {
        self.model.as_deref().unwrap_or("MCT display")
    }

    /// The format setting and what is in use, e.g. `auto (RGB24)`.
    pub fn format_summary(&self) -> String {
        match (&self.format, &self.pixel_format) {
            (Some(f), Some(p)) if p != "-" => format!("{f} ({p})"),
            (Some(f), _) => f.clone(),
            (None, Some(p)) => p.clone(),
            (None, None) => "-".into(),
        }
    }
}

/// Something about a device the user should know.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Warning {
    /// A T6 on a USB 2 link.
    Usb2,
    /// Transfer errors (and, on a T6, device restarts) since connect.
    TransferErrors { errors: u64, recoveries: u64 },
}

impl Warning {
    /// The full text, for the window and mctctl.
    pub fn text(&self) -> String {
        match self {
            Warning::Usb2 => "Connected over USB 2: modes are limited to 2048 pixels wide and \
                 motion is slower. Plug the adapter into a USB 3 port directly, not through a \
                 USB 2 hub or extension."
                .into(),
            Warning::TransferErrors { errors, recoveries } => {
                let mut s = format!("Transfer errors since connect: {errors}");
                if *recoveries > 0 {
                    s.push_str(&format!(", device restarts: {recoveries}"));
                }
                s
            }
        }
    }

    /// One short line, for the tray menu.
    pub fn short(&self) -> String {
        match self {
            Warning::Usb2 => "USB 2 link".into(),
            Warning::TransferErrors { errors, .. } => format!("Transfer errors: {errors}"),
        }
    }

    /// USB 2 is a setup problem; counted errors are only worth a note.
    pub fn is_serious(&self) -> bool {
        matches!(self, Warning::Usb2)
    }
}

impl Info {
    pub fn warnings(&self) -> Vec<Warning> {
        let mut out = Vec::new();
        if self.chip == Chip::T6 && self.usb_speed.as_deref() == Some("usb2") {
            out.push(Warning::Usb2);
        }
        let errors = self.errors.unwrap_or(0);
        let recoveries = self.recoveries.unwrap_or(0);
        if errors > 0 || recoveries > 0 {
            out.push(Warning::TransferErrors { errors, recoveries });
        }
        out
    }
}

/// Strips the `cardN-` prefix of a DRM connector name.
pub fn short_connector(name: &str) -> &str {
    if let Some(rest) = name.strip_prefix("card") {
        let digits = rest.bytes().take_while(u8::is_ascii_digit).count();
        if digits > 0 && rest.as_bytes().get(digits) == Some(&b'-') {
            return &rest[digits + 1..];
        }
    }
    name
}

/// One device bound to mct.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Device {
    pub interface: String,
    dir: PathBuf,
    root: PathBuf,
}

/// Bound devices under `sysfs_root()`, in interface order.
pub fn list_devices() -> Vec<Device> {
    list_devices_in(&sysfs_root())
}

pub fn list_devices_in(root: &Path) -> Vec<Device> {
    let Ok(entries) = fs::read_dir(driver_dir(root)) else {
        return Vec::new();
    };
    let mut devs: Vec<Device> = entries
        .flatten()
        .filter_map(|e| {
            let dir = e.path().join("mct");
            if !dir.is_dir() {
                return None;
            }
            Some(Device {
                interface: e.file_name().to_string_lossy().into_owned(),
                dir,
                root: root.to_path_buf(),
            })
        })
        .collect();
    devs.sort_by(|a, b| natural_cmp(&a.interface, &b.interface));
    devs
}

/// Compares so that `2-10:1.0` comes after `2-2:1.0`.
pub(crate) fn natural_cmp(a: &str, b: &str) -> Ordering {
    fn chunks(s: &str) -> Vec<(bool, &str)> {
        let mut out = Vec::new();
        let mut start = 0;
        let b = s.as_bytes();
        for i in 1..=b.len() {
            if i == b.len() || b[i].is_ascii_digit() != b[start].is_ascii_digit() {
                out.push((b[start].is_ascii_digit(), &s[start..i]));
                start = i;
            }
        }
        out
    }
    let (ca, cb) = (chunks(a), chunks(b));
    for (x, y) in ca.iter().zip(cb.iter()) {
        let ord = match (x, y) {
            ((true, x), (true, y)) => x
                .parse::<u64>()
                .unwrap_or(0)
                .cmp(&y.parse::<u64>().unwrap_or(0))
                .then(x.cmp(y)),
            ((_, x), (_, y)) => x.cmp(y),
        };
        if ord != Ordering::Equal {
            return ord;
        }
    }
    ca.len().cmp(&cb.len())
}

/// The driver writes `-` for "none" (no connector yet, no backlight).
fn none_if_dash(v: Option<String>) -> Option<String> {
    v.filter(|s| !s.is_empty() && s != "-")
}

fn parse<T: std::str::FromStr>(v: Option<String>) -> Option<T> {
    v.and_then(|s| s.trim().parse().ok())
}

/// Parses the values `all_modes` accepts: 0/1, on/off, true/false, yes/no.
pub fn parse_bool(value: &str) -> Option<bool> {
    match value.trim().to_ascii_lowercase().as_str() {
        "1" | "on" | "true" | "yes" => Some(true),
        "0" | "off" | "false" | "no" => Some(false),
        _ => None,
    }
}

impl Device {
    /// The device's `mct` directory.
    pub fn dir(&self) -> &Path {
        &self.dir
    }

    fn gone(&self) -> Error {
        Error::Gone(self.interface.clone())
    }

    fn read_path(&self, path: &Path) -> Result<Option<String>, Error> {
        match fs::read_to_string(path) {
            Ok(s) => Ok(Some(s.trim_end_matches('\n').to_string())),
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                if self.dir.is_dir() {
                    Ok(None)
                } else {
                    Err(self.gone())
                }
            }
            Err(e) if e.raw_os_error() == Some(ENODEV) => Err(self.gone()),
            Err(err) => Err(Error::Io {
                path: path.to_path_buf(),
                err,
            }),
        }
    }

    /// One file of the device's directory; `None` if this device lacks it.
    pub fn read(&self, key: &str) -> Result<Option<String>, Error> {
        self.read_path(&self.dir.join(key))
    }

    pub fn chip(&self) -> Result<Chip, Error> {
        Ok(Chip::parse(self.read("chip")?.as_deref().unwrap_or("")))
    }

    /// Reads every file at once.
    pub fn info(&self) -> Result<Info, Error> {
        let r = |k: &str| self.read(k);
        let mut info = Info {
            interface: self.interface.clone(),
            chip: Chip::parse(r("chip")?.as_deref().unwrap_or("")),
            model: none_if_dash(r("model")?),
            connector: none_if_dash(r("connector")?),
            mode: r("mode")?,
            pixel_format: r("pixel_format")?,
            feature_level: parse(r("feature_level")?),
            vram_mb: parse(r("vram_mb")?),
            usb_speed: r("usb_speed")?,
            updates: parse(r("updates")?),
            bytes: parse(r("bytes")?),
            errors: parse(r("errors")?),
            recoveries: parse(r("recoveries")?),
            format: r("format")?,
            all_modes: parse(r("all_modes")?),
            lossy: parse(r("lossy")?),
            backlight: none_if_dash(r("backlight")?),
            brightness: None,
            max_brightness: None,
        };
        if let Some(dir) = info.backlight.as_deref().map(|b| self.backlight_path(b)) {
            info.brightness = parse(fs::read_to_string(dir.join("brightness")).ok());
            info.max_brightness = parse(fs::read_to_string(dir.join("max_brightness")).ok());
        }
        Ok(info)
    }

    /// One value by key (see `KEYS`).
    pub fn get(&self, key: &str) -> Result<String, Error> {
        if !KEYS.contains(&key) {
            return Err(Error::UnknownKey(key.into()));
        }
        let info = self.info()?;
        info.value(key).ok_or(Error::Unsupported {
            key: key.into(),
            chip: info.chip,
        })
    }

    fn backlight_path(&self, name: &str) -> PathBuf {
        self.root.join("sys/class/backlight").join(name)
    }

    fn backlight_dir(&self) -> Result<PathBuf, Error> {
        match none_if_dash(self.read("backlight")?) {
            Some(name) => Ok(self.backlight_path(&name)),
            None => Err(Error::Unsupported {
                key: "brightness".into(),
                chip: self.chip()?,
            }),
        }
    }

    /// Current and maximum brightness of a T2's backlight.
    pub fn brightness(&self) -> Result<(u32, u32), Error> {
        let dir = self.backlight_dir()?;
        let read = |f: &str| -> Result<u32, Error> {
            let path = dir.join(f);
            let s = fs::read_to_string(&path).map_err(|err| Error::Io {
                path: path.clone(),
                err,
            })?;
            s.trim()
                .parse()
                .map_err(|_| Error::Invalid(format!("{}: not a number", path.display())))
        };
        Ok((read("brightness")?, read("max_brightness")?))
    }

    /// Sets one of `SETTABLE` from text, validating it first.
    pub fn set(&self, key: &str, value: &str) -> Result<(), Error> {
        match key {
            "format" => self.set_format(value.trim()),
            "all_modes" => match parse_bool(value) {
                Some(b) => self.set_all_modes(b),
                None => Err(Error::Invalid(format!(
                    "invalid value \"{value}\" for all_modes (use 0 or 1)"
                ))),
            },
            "lossy" => match value.trim().parse::<u32>() {
                Ok(v) => self.set_lossy(v),
                Err(_) => Err(Error::Invalid(format!(
                    "invalid value \"{value}\" for lossy (use 0 to {LOSSY_MAX})"
                ))),
            },
            "brightness" => match value.trim().parse::<u32>() {
                Ok(v) => self.set_brightness(v),
                Err(_) => Err(Error::Invalid(format!(
                    "invalid value \"{value}\" for brightness (use a number, 0 = off)"
                ))),
            },
            k if KEYS.contains(&k) => Err(Error::Invalid(format!(
                "{k} is read-only (settable: {})",
                SETTABLE.join(", ")
            ))),
            k => Err(Error::UnknownKey(k.into())),
        }
    }

    fn require(&self, key: &str) -> Result<(), Error> {
        if self.dir.join(key).exists() {
            Ok(())
        } else if !self.dir.is_dir() {
            Err(self.gone())
        } else {
            Err(Error::Unsupported {
                key: key.into(),
                chip: self.chip()?,
            })
        }
    }

    pub fn set_format(&self, format: &str) -> Result<(), Error> {
        self.require("format")?;
        if !FORMATS.contains(&format) {
            return Err(Error::Invalid(format!(
                "invalid format \"{format}\" (use one of {})",
                FORMATS.join(", ")
            )));
        }
        self.write_key("format", format)
    }

    pub fn set_all_modes(&self, on: bool) -> Result<(), Error> {
        self.require("all_modes")?;
        self.write_key("all_modes", if on { "1" } else { "0" })
    }

    pub fn set_lossy(&self, level: u32) -> Result<(), Error> {
        self.require("lossy")?;
        if level > LOSSY_MAX {
            return Err(Error::Invalid(format!(
                "invalid lossy level {level} (use 0 to {LOSSY_MAX})"
            )));
        }
        self.write_key("lossy", &level.to_string())
    }

    /// Sets a T2's backlight; 0 turns it off.
    pub fn set_brightness(&self, value: u32) -> Result<(), Error> {
        let dir = self.backlight_dir()?;
        let (_, max) = self.brightness()?;
        if value > max {
            return Err(Error::Invalid(format!(
                "invalid brightness {value} (use 0 to {max}, 0 = off)"
            )));
        }
        let path = dir.join("brightness");
        match write_file(&path, &value.to_string()) {
            Err(Error::Permission { .. }) if self.logind_possible() => {
                let name = dir
                    .file_name()
                    .unwrap_or_default()
                    .to_string_lossy()
                    .into_owned();
                logind_set_brightness(&name, value).map_err(|_| Error::Permission {
                    path,
                    hint: PERMISSION_HINT,
                })
            }
            r => r,
        }
    }

    fn write_key(&self, key: &str, value: &str) -> Result<(), Error> {
        match write_file(&self.dir.join(key), value) {
            Err(Error::Io { err, .. }) if err.raw_os_error() == Some(ENODEV) => Err(self.gone()),
            Err(Error::Io { err, .. }) if err.raw_os_error() == Some(EINVAL) => Err(
                Error::Invalid(format!("the driver rejected \"{value}\" for {key}")),
            ),
            r => r,
        }
    }

    /// Whether `set(key, ..)` can work without root: the file is writable
    /// (or, for brightness, logind can set it).
    pub fn can_write(&self, key: &str) -> bool {
        let path = if key == "brightness" {
            match self.backlight_dir() {
                Ok(d) => d.join("brightness"),
                Err(_) => return false,
            }
        } else {
            self.dir.join(key)
        };
        // opening without writing anything checks the permission and
        // leaves the attribute alone
        let ok = fs::OpenOptions::new().write(true).open(&path).is_ok();
        ok || (key == "brightness" && path.exists() && self.logind_possible())
    }

    /// logind's SetBrightness works for the user of the active session
    /// without any udev rule; only tried against the real /sys.
    fn logind_possible(&self) -> bool {
        self.root == Path::new("/") && which("busctl")
    }
}

fn write_file(path: &Path, value: &str) -> Result<(), Error> {
    let res = fs::OpenOptions::new()
        .write(true)
        .truncate(true)
        .open(path)
        .and_then(|mut f| f.write_all(format!("{value}\n").as_bytes()));
    match res {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == io::ErrorKind::PermissionDenied => Err(Error::Permission {
            path: path.to_path_buf(),
            hint: PERMISSION_HINT,
        }),
        Err(err) => Err(Error::Io {
            path: path.to_path_buf(),
            err,
        }),
    }
}

fn which(prog: &str) -> bool {
    std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).any(|d| d.join(prog).is_file()))
        .unwrap_or(false)
}

fn logind_set_brightness(name: &str, value: u32) -> io::Result<()> {
    let status = Command::new("busctl")
        .args([
            "call",
            "org.freedesktop.login1",
            "/org/freedesktop/login1/session/auto",
            "org.freedesktop.login1.Session",
            "SetBrightness",
            "ssu",
            "backlight",
            name,
            &value.to_string(),
        ])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other("busctl failed"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn short_connector_names() {
        assert_eq!(short_connector("card2-HDMI-A-3"), "HDMI-A-3");
        assert_eq!(short_connector("HDMI-A-3"), "HDMI-A-3");
        assert_eq!(short_connector("card-x"), "card-x");
        assert_eq!(short_connector("card12-USB-1"), "USB-1");
    }

    #[test]
    fn natural_order() {
        let mut v = vec!["2-10:1.0", "2-2:1.0", "10-1:1.0", "3-1:1.0"];
        v.sort_by(|a, b| natural_cmp(a, b));
        assert_eq!(v, ["2-2:1.0", "2-10:1.0", "3-1:1.0", "10-1:1.0"]);
    }

    #[test]
    fn bools() {
        assert_eq!(parse_bool("on"), Some(true));
        assert_eq!(parse_bool(" 0\n"), Some(false));
        assert_eq!(parse_bool("2"), None);
    }
}

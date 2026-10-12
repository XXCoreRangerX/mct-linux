// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Settings remembered per device, applied again when the device comes back.
//!
//! The driver starts every device with the module's defaults, also after a
//! replug or a T6 restart. Changes made in the window, the tray or with
//! `mctctl set` are saved to `~/.config/mct/settings.toml` (or
//! `$MCT_SETTINGS`), and [`Reapplier`] writes them back whenever a saved
//! device appears or is probed again.

use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::device::{Device, Error, Info};
use crate::ident;
use crate::usb::Display;

/// The settings of one device. `None`: not saved, the driver's default
/// stays.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Saved {
    /// What the device was, for reading the file.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub format: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub all_modes: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lossy: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub motion: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub brightness: Option<u32>,
}

impl Saved {
    /// The settings a device has now.
    pub fn from_info(name: &str, info: &Info) -> Saved {
        Saved {
            name: name.to_string(),
            format: info.format.clone(),
            all_modes: info.all_modes.map(|a| a != 0),
            lossy: info.lossy,
            motion: info.motion.clone(),
            brightness: info.brightness,
        }
    }

    /// `(key, value)` pairs as `Device::set` takes them.
    pub fn entries(&self) -> Vec<(&'static str, String)> {
        let mut v = Vec::new();
        if let Some(f) = &self.format {
            v.push(("format", f.clone()));
        }
        if let Some(a) = self.all_modes {
            v.push(("all_modes", if a { "1" } else { "0" }.to_string()));
        }
        if let Some(l) = self.lossy {
            v.push(("lossy", l.to_string()));
        }
        if let Some(m) = &self.motion {
            v.push(("motion", m.clone()));
        }
        if let Some(b) = self.brightness {
            v.push(("brightness", b.to_string()));
        }
        v
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Store {
    #[serde(default)]
    pub devices: BTreeMap<String, Saved>,
}

const HEADER: &str = "# MCT Display Manager / mctctl: settings per device, applied again when\n\
# the device is connected. Written by the app; safe to edit or delete.\n\n";

/// `$MCT_SETTINGS`, else `$XDG_CONFIG_HOME/mct/settings.toml`, else
/// `~/.config/mct/settings.toml`. With a fake sysfs tree (`MCT_SYSFS_ROOT`)
/// the file is `settings.toml` in that tree, so tests never touch the real
/// one.
pub fn path() -> Option<PathBuf> {
    if let Some(p) = std::env::var_os("MCT_SETTINGS").filter(|p| !p.is_empty()) {
        return Some(PathBuf::from(p));
    }
    if let Some(root) = std::env::var_os("MCT_SYSFS_ROOT").filter(|p| !p.is_empty()) {
        return Some(PathBuf::from(root).join("settings.toml"));
    }
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|p| !p.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))?;
    Some(config.join("mct").join("settings.toml"))
}

impl Store {
    /// The saved settings; an empty store if there is no file yet.
    pub fn load_from(path: &Path) -> Result<Store, Error> {
        match fs::read_to_string(path) {
            Ok(s) => toml::from_str(&s).map_err(|e| {
                Error::Invalid(format!("{}: {}", path.display(), e.message()))
            }),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(Store::default()),
            Err(err) => Err(Error::Io {
                path: path.to_path_buf(),
                err,
            }),
        }
    }

    pub fn load() -> Result<Store, Error> {
        match path() {
            Some(p) => Store::load_from(&p),
            None => Ok(Store::default()),
        }
    }

    /// Writes the file whole (to a temporary file, then renamed).
    pub fn save_to(&self, path: &Path) -> Result<(), Error> {
        let io = |err| Error::Io {
            path: path.to_path_buf(),
            err,
        };
        if let Some(dir) = path.parent() {
            fs::create_dir_all(dir).map_err(io)?;
        }
        let body = toml::to_string_pretty(self)
            .map_err(|e| Error::Invalid(format!("settings: {e}")))?;
        let tmp = path.with_extension("toml.tmp");
        fs::write(&tmp, format!("{HEADER}{body}")).map_err(io)?;
        fs::rename(&tmp, path).map_err(io)
    }

    pub fn save(&self) -> Result<(), Error> {
        match path() {
            Some(p) => self.save_to(&p),
            None => Err(Error::Invalid("no home directory to save settings in".into())),
        }
    }
}

/// What identifies a device across replugs: its USB IDs and a serial number
/// (the USB device's, the chip's, or for the DTU-1031X its pen's), else the
/// USB port it is plugged into (the T6 adapters have no serial number).
pub fn device_key(display: &Display) -> String {
    let root = &display.root;
    let dev = display
        .interface
        .split(':')
        .next()
        .unwrap_or(&display.interface);
    let dir = root.join("sys/bus/usb/devices").join(dev);
    let read = |p: PathBuf| {
        fs::read_to_string(p)
            .ok()
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty() && s != "-")
    };
    let id = format!(
        "{}:{}",
        read(dir.join("idVendor")).unwrap_or_default(),
        read(dir.join("idProduct")).unwrap_or_default()
    );
    let serial = read(dir.join("serial"))
        .or_else(|| {
            display
                .device
                .as_ref()
                .and_then(|d| read(d.dir().join("serial")))
        })
        .or_else(|| ident::pen_serial(root, &display.interface));
    match serial {
        Some(s) => format!("{id}/serial-{s}"),
        None => format!("{id}/port-{dev}"),
    }
}

/// The current settings of `display` become its saved ones.
pub fn remember(display: &Display) -> Result<(), Error> {
    let Some(dev) = &display.device else {
        return Ok(());
    };
    let info = dev.info()?;
    let name = ident::product(display, Some(&info))
        .map(|p| p.short.to_string())
        .or_else(|| info.model.clone())
        .unwrap_or_default();
    let mut store = Store::load()?;
    let saved = Saved::from_info(&name, &info);
    let key = device_key(display);
    if store.devices.get(&key) != Some(&saved) {
        store.devices.insert(key, saved);
        store.save()?;
    }
    Ok(())
}

/// Removes the saved settings of `display`; false if there were none.
pub fn forget(display: &Display) -> Result<bool, Error> {
    let mut store = Store::load()?;
    let found = store.devices.remove(&device_key(display)).is_some();
    if found {
        store.save()?;
    }
    Ok(found)
}

/// Writes the saved settings of `display` that differ from what it has now.
/// Returns the keys that were set.
pub fn apply(display: &Display, store: &Store) -> Result<Vec<&'static str>, Error> {
    let Some(dev) = &display.device else {
        return Ok(Vec::new());
    };
    let Some(saved) = store.devices.get(&device_key(display)) else {
        return Ok(Vec::new());
    };
    apply_saved(dev, saved)
}

fn apply_saved(dev: &Device, saved: &Saved) -> Result<Vec<&'static str>, Error> {
    let info = dev.info()?;
    let mut done = Vec::new();
    let mut first_err = None;
    for (key, value) in saved.entries() {
        // only what this device has and what differs
        let Some(cur) = info.value(key) else { continue };
        if cur == value || (key == "all_modes" && (cur != "0") == (value == "1")) {
            continue;
        }
        match dev.set(key, &value) {
            Ok(()) => done.push(key),
            Err(e) => {
                first_err.get_or_insert(e);
            }
        }
    }
    match first_err {
        Some(e) if done.is_empty() => Err(e),
        _ => Ok(done),
    }
}

/// Watches for devices that appear or are probed again (the driver's
/// directory is created anew on each probe) and applies their saved
/// settings. Call [`Reapplier::scan`] regularly, e.g. once a second.
#[derive(Default)]
pub struct Reapplier {
    seen: HashMap<String, u64>,
}

impl Reapplier {
    pub fn new() -> Reapplier {
        Reapplier::default()
    }

    /// Applies saved settings to new or re-probed devices among `displays`.
    /// Returns what was applied, per display name, and errors.
    pub fn scan(&mut self, displays: &[Display]) -> Vec<(String, Result<Vec<&'static str>, Error>)> {
        let mut now = HashMap::new();
        let mut fresh = Vec::new();
        for d in displays {
            let Some(dev) = &d.device else { continue };
            let Ok(meta) = fs::metadata(dev.dir()) else { continue };
            let ino = meta.ino();
            if self.seen.get(&d.interface) != Some(&ino) {
                fresh.push(d);
            }
            now.insert(d.interface.clone(), ino);
        }
        self.seen = now;
        if fresh.is_empty() {
            return Vec::new();
        }
        let store = match Store::load() {
            Ok(s) => s,
            Err(e) => return vec![(String::new(), Err(e))],
        };
        fresh
            .into_iter()
            .filter(|d| store.devices.contains_key(&device_key(d)))
            .map(|d| (d.name().to_string(), apply(d, &store)))
            .collect()
    }
}

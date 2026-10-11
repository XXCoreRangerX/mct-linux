// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Every MCT display on USB, whichever driver (if any) handles it.

use std::fs;
use std::path::Path;

use serde::Serialize;

use crate::device::{
    list_devices_in, natural_cmp, short_connector, sysfs_root, Chip, Device, Error, Info, Warning,
};

pub const VENDOR: &str = "0711";

/// Product IDs the driver handles, and their chip.
pub const PRODUCTS: &[(&str, Chip)] = &[
    ("5209", Chip::T2),
    ("520d", Chip::T2),
    ("5600", Chip::T6),
    ("5601", Chip::T6),
];

pub const LEGACY_DAEMON: &str = "wacom-t2d";

/// Which driver handles a display.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum Route {
    /// Bound to mct.
    Kernel,
    /// Nothing bound and the mct module isn't loaded.
    NotLoaded,
    /// Nothing bound although mct is loaded.
    NotBound,
    /// The tablet driven by wacom-t2d over EVDI.
    Legacy,
    /// Some other driver holds the interface.
    Other(String),
}

impl Route {
    pub fn label(&self) -> String {
        match self {
            Route::Kernel => "Kernel driver (mct)".into(),
            Route::NotLoaded => "Kernel driver not loaded".into(),
            Route::NotBound => "Not bound".into(),
            Route::Legacy => "Legacy: wacom-t2d over EVDI".into(),
            Route::Other(d) => format!("Other driver: {d}"),
        }
    }

    /// Short form for tables.
    pub fn short(&self) -> String {
        match self {
            Route::Kernel => "mct".into(),
            Route::NotLoaded => "not-loaded".into(),
            Route::NotBound => "not-bound".into(),
            Route::Legacy => "legacy".into(),
            Route::Other(d) => d.clone(),
        }
    }

    pub fn hint(&self) -> Option<&'static str> {
        match self {
            Route::NotLoaded => {
                Some("Load it with \"sudo modprobe mct\" (on NixOS: hardware.mct.enable = true).")
            }
            Route::NotBound => Some("Replug the device, or switch it to the kernel driver."),
            _ => None,
        }
    }
}

/// One MCT display on USB.
#[derive(Clone, Debug, PartialEq)]
pub struct Display {
    /// USB interface, e.g. `2-2:1.0`.
    pub interface: String,
    pub chip: Chip,
    /// USB product string.
    pub product: Option<String>,
    /// `usb2` or `usb3` (or `usb1`), from the USB device's speed.
    pub usb_speed: Option<String>,
    pub route: Route,
    /// The mct sysfs directory, when bound to mct.
    pub device: Option<Device>,
}

impl Display {
    pub fn is_tablet(&self) -> bool {
        self.chip == Chip::T2
    }

    pub fn name(&self) -> &str {
        self.product.as_deref().unwrap_or("MCT display")
    }
}

/// A display with what the driver reports, for showing it.
#[derive(Clone, Debug, PartialEq)]
pub struct Snapshot {
    pub display: Display,
    /// Present when bound to mct and readable.
    pub info: Option<Info>,
}

impl Snapshot {
    pub fn take(display: Display) -> Snapshot {
        let info = display.device.as_ref().and_then(|d| d.info().ok());
        Snapshot { display, info }
    }

    pub fn name(&self) -> &str {
        match &self.info {
            Some(i) => i.name(),
            None => self.display.name(),
        }
    }

    /// Warnings from the driver's values; without the driver only the USB
    /// link is known.
    pub fn warnings(&self) -> Vec<Warning> {
        match &self.info {
            Some(i) => i.warnings(),
            None if self.display.chip == Chip::T6
                && self.display.usb_speed.as_deref() == Some("usb2") =>
            {
                vec![Warning::Usb2]
            }
            None => Vec::new(),
        }
    }

    /// Connector (short) when bound, otherwise the interface.
    pub fn place(&self) -> &str {
        self.info
            .as_ref()
            .and_then(|i| i.connector_short())
            .unwrap_or(&self.display.interface)
    }
}

fn read(path: &Path) -> Option<String> {
    let s = fs::read_to_string(path).ok()?;
    let s = s.trim();
    (!s.is_empty()).then(|| s.to_string())
}

fn speed_name(mbps: &str) -> Option<String> {
    let v: f64 = mbps.trim().parse().ok()?;
    Some(
        if v >= 5000.0 {
            "usb3"
        } else if v >= 480.0 {
            "usb2"
        } else {
            "usb1"
        }
        .into(),
    )
}

/// Whether a process with this name runs (any user).
pub fn process_running(root: &Path, name: &str) -> bool {
    let Ok(entries) = fs::read_dir(root.join("proc")) else {
        return false;
    };
    entries.flatten().any(|e| {
        e.file_name()
            .to_string_lossy()
            .bytes()
            .all(|b| b.is_ascii_digit())
            && read(&e.path().join("comm")).as_deref() == Some(name)
    })
}

pub fn module_loaded(root: &Path, module: &str) -> bool {
    root.join("sys/module").join(module).is_dir()
}

/// All MCT displays under `sysfs_root()`.
pub fn discover() -> Vec<Display> {
    discover_in(&sysfs_root())
}

pub fn discover_in(root: &Path) -> Vec<Display> {
    let bound = list_devices_in(root);
    let mut out: Vec<Display> = Vec::new();
    let usb = root.join("sys/bus/usb/devices");
    let mut usb_entries: Vec<String> = fs::read_dir(&usb)
        .map(|r| {
            r.flatten()
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect()
        })
        .unwrap_or_default();
    usb_entries.sort_by(|a, b| natural_cmp(a, b));

    let legacy_running = || process_running(root, LEGACY_DAEMON) && module_loaded(root, "evdi");

    for dev in usb_entries.iter().filter(|n| !n.contains(':')) {
        let dir = usb.join(dev);
        if read(&dir.join("idVendor")).as_deref() != Some(VENDOR) {
            continue;
        }
        let Some(product_id) = read(&dir.join("idProduct")) else {
            continue;
        };
        let Some(&(_, chip)) = PRODUCTS.iter().find(|(p, _)| *p == product_id) else {
            continue;
        };
        // the display function is interface 0 of the active configuration
        let Some(interface) = usb_entries
            .iter()
            .find(|n| n.starts_with(&format!("{dev}:")) && n.ends_with(".0"))
            .cloned()
        else {
            continue;
        };
        let driver = fs::read_link(usb.join(&interface).join("driver"))
            .ok()
            .and_then(|p| p.file_name().map(|f| f.to_string_lossy().into_owned()));
        let device = bound.iter().find(|d| d.interface == interface).cloned();
        let route = if driver.as_deref() == Some("mct") || device.is_some() {
            Route::Kernel
        } else if chip == Chip::T2 && legacy_running() {
            Route::Legacy
        } else if let Some(d) = driver {
            Route::Other(d)
        } else if module_loaded(root, "mct") {
            Route::NotBound
        } else {
            Route::NotLoaded
        };
        out.push(Display {
            interface,
            chip,
            product: read(&dir.join("product")),
            usb_speed: read(&dir.join("speed")).and_then(|s| speed_name(&s)),
            route,
            device,
        });
    }

    // bound devices the USB scan missed (IDs added to the driver later)
    for d in bound {
        if out.iter().any(|x| x.interface == d.interface) {
            continue;
        }
        let info = d.info().ok();
        out.push(Display {
            interface: d.interface.clone(),
            chip: info.as_ref().map(|i| i.chip).unwrap_or_default(),
            product: info.as_ref().and_then(|i| i.model.clone()),
            usb_speed: info.as_ref().and_then(|i| i.usb_speed.clone()),
            route: Route::Kernel,
            device: Some(d),
        });
    }
    out.sort_by(|a, b| natural_cmp(&a.interface, &b.interface));
    out
}

/// Picks a display by interface, connector (`HDMI-A-3` or
/// `card2-HDMI-A-3`) or index into `displays`; without a name, the only one.
pub fn select<'a>(displays: &'a [Display], spec: Option<&str>) -> Result<&'a Display, Error> {
    let Some(spec) = spec else {
        return match displays.len() {
            0 => Err(Error::NoDevices),
            1 => Ok(&displays[0]),
            n => Err(Error::NeedDevice(n)),
        };
    };
    if let Some(d) = displays.iter().find(|d| d.interface == spec) {
        return Ok(d);
    }
    let by_connector: Vec<&Display> = displays
        .iter()
        .filter(|d| {
            let c = d
                .device
                .as_ref()
                .and_then(|dev| dev.read("connector").ok().flatten());
            c.is_some_and(|c| c != "-" && (c == spec || short_connector(&c) == spec))
        })
        .collect();
    match by_connector.len() {
        1 => return Ok(by_connector[0]),
        0 => {}
        _ => {
            return Err(Error::Ambiguous(
                spec.into(),
                by_connector.iter().map(|d| d.interface.clone()).collect(),
            ))
        }
    }
    if !spec.is_empty() && spec.bytes().all(|b| b.is_ascii_digit()) {
        if let Some(d) = spec.parse::<usize>().ok().and_then(|i| displays.get(i)) {
            return Ok(d);
        }
    }
    if displays.is_empty() {
        return Err(Error::NoDevices);
    }
    Err(Error::NotFound(spec.into()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn speeds() {
        assert_eq!(speed_name("480").as_deref(), Some("usb2"));
        assert_eq!(speed_name("5000").as_deref(), Some("usb3"));
        assert_eq!(speed_name("12").as_deref(), Some("usb1"));
        assert_eq!(speed_name("x"), None);
    }
}

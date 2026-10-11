// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Everything that identifies a display: the product, the chip and its
//! firmware, the USB device, the driver, the monitor (from its EDID) and,
//! on the tablet, the pen digitizer. Read from sysfs, so most of it is known
//! without the kernel driver too.

use std::fs;
use std::path::{Path, PathBuf};

use serde::Serialize;

use crate::device::Info;
use crate::usb::{Display, VENDOR};
use crate::Chip;

/// One line: a stable key (for `--json`), a label and the value.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Field {
    pub key: &'static str,
    pub label: &'static str,
    pub value: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Section {
    pub key: &'static str,
    pub title: &'static str,
    pub fields: Vec<Field>,
}

/// Products known by the board name ("project code") the T6 reports.
/// The names are from MCT's firmware packages and the vendors' product pages.
const BOARDS: &[(&str, Product)] = &[
    (
        "JUA254H",
        Product {
            short: "j5create JUA254",
            full: "j5create JUA254 USB 3.0 to HDMI Display Adapter",
        },
    ),
    (
        "A8613HV",
        Product {
            short: "MCT U3-A8613",
            full: "MCT U3-A8613 USB 3.0 to HDMI and VGA Adapter",
        },
    ),
    (
        "DTU-1031AX",
        Product {
            short: "Wacom DTU-1031AX",
            full: "Wacom DTU-1031AX pen display",
        },
    ),
];

/// Products known by their USB IDs and product string ("" matches any).
const USB_PRODUCTS: &[(&str, &str, &str, Product)] = &[(
    VENDOR,
    "520d",
    "DTU-1031X",
    Product {
        short: "Wacom DTU-1031X",
        full: "Wacom DTU-1031X pen display",
    },
)];

/// A product's name, short for titles and full for the details.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Product {
    pub short: &'static str,
    pub full: &'static str,
}

const USB_VENDORS: &[(&str, &str)] = &[
    (VENDOR, "Magic Control Technology Corp."),
    ("056a", "Wacom Co., Ltd."),
];

/// EDID manufacturer IDs of common monitor makers.
const PNP_IDS: &[(&str, &str)] = &[
    ("ACR", "Acer"),
    ("AOC", "AOC"),
    ("AUO", "AU Optronics"),
    ("AUS", "ASUS"),
    ("BNQ", "BenQ"),
    ("BOE", "BOE"),
    ("CMN", "Chimei Innolux"),
    ("DEL", "Dell"),
    ("ENC", "EIZO"),
    ("GBT", "Gigabyte"),
    ("GSM", "LG"),
    ("HPN", "HP"),
    ("HWP", "HP"),
    ("IVM", "iiyama"),
    ("LEN", "Lenovo"),
    ("LGD", "LG Display"),
    ("MEI", "Panasonic"),
    ("MSI", "MSI"),
    ("NEC", "NEC"),
    ("PHL", "Philips"),
    ("SAM", "Samsung"),
    ("SDC", "Samsung Display"),
    ("SHP", "Sharp"),
    ("SNY", "Sony"),
    ("VSC", "ViewSonic"),
    ("WAC", "Wacom"),
    ("XMI", "Xiaomi"),
];

fn lookup<'a>(table: &[(&str, &'a str)], key: &str) -> Option<&'a str> {
    table
        .iter()
        .find(|(k, _)| k.eq_ignore_ascii_case(key))
        .map(|(_, v)| *v)
}

fn read(path: &Path) -> Option<String> {
    let s = fs::read_to_string(path).ok()?;
    let s = s.trim();
    (!s.is_empty() && s != "-").then(|| s.to_string())
}

struct Builder {
    fields: Vec<Field>,
}

impl Builder {
    fn new() -> Builder {
        Builder { fields: Vec::new() }
    }

    fn add(&mut self, key: &'static str, label: &'static str, value: Option<String>) {
        if let Some(value) = value.filter(|v| !v.is_empty()) {
            self.fields.push(Field { key, label, value });
        }
    }

    fn done(self, key: &'static str, title: &'static str, out: &mut Vec<Section>) {
        if !self.fields.is_empty() {
            out.push(Section {
                key,
                title,
                fields: self.fields,
            });
        }
    }
}

/// The product name, if the board or the USB IDs say which product it is.
pub fn product(display: &Display, info: Option<&Info>) -> Option<Product> {
    if let Some(p) = info
        .and_then(|i| i.board.as_deref())
        .and_then(|b| BOARDS.iter().find(|(k, _)| k.eq_ignore_ascii_case(b)))
    {
        return Some(p.1);
    }
    let dir = usb_device_dir(&display.root, &display.interface);
    let (v, p) = (read(&dir.join("idVendor"))?, read(&dir.join("idProduct"))?);
    let product = read(&dir.join("product")).unwrap_or_default();
    USB_PRODUCTS
        .iter()
        .find(|(pv, pp, ps, _)| *pv == v && *pp == p && (ps.is_empty() || *ps == product))
        .map(|(.., p)| *p)
}

/// `2-2` for interface `2-2:1.0`.
fn usb_device_dir(root: &Path, interface: &str) -> PathBuf {
    let dev = interface.split(':').next().unwrap_or(interface);
    root.join("sys/bus/usb/devices").join(dev)
}

fn speed_text(mbps: &str) -> String {
    let name = match mbps {
        "1.5" => "Low Speed",
        "12" => "Full Speed",
        "480" => "High Speed",
        "5000" => "SuperSpeed",
        "10000" => "SuperSpeed+",
        "20000" => "SuperSpeed+ 2x2",
        _ => return format!("{mbps} Mbit/s"),
    };
    format!("{name}, {mbps} Mbit/s")
}

/// bcdDevice `1010` reads as `10.10`.
fn bcd_text(bcd: &str) -> String {
    if bcd.len() == 4 && bcd.bytes().all(|b| b.is_ascii_hexdigit()) {
        let major = bcd[..2].trim_start_matches('0');
        format!("{}.{}", if major.is_empty() { "0" } else { major }, &bcd[2..])
    } else {
        bcd.into()
    }
}

fn usb_id(dir: &Path) -> Option<String> {
    let v = read(&dir.join("idVendor"))?;
    let p = read(&dir.join("idProduct"))?;
    Some(match lookup(USB_VENDORS, &v) {
        Some(name) => format!("{v}:{p} ({name})"),
        None => format!("{v}:{p}"),
    })
}

/// The monitor's identity from an EDID (base block).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Edid {
    pub vendor: String,
    pub product_code: u16,
    pub serial: u32,
    pub name: Option<String>,
    pub serial_text: Option<String>,
    pub week: u8,
    pub year: u16,
    pub version: (u8, u8),
    pub size_cm: (u8, u8),
}

pub fn parse_edid(e: &[u8]) -> Option<Edid> {
    if e.len() < 128 || e[..8] != [0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0] {
        return None;
    }
    let id = u16::from_be_bytes([e[8], e[9]]);
    let letter = |shift: u16| (b'A' - 1 + ((id >> shift) & 0x1f) as u8) as char;
    let mut out = Edid {
        vendor: [letter(10), letter(5), letter(0)].iter().collect(),
        product_code: u16::from_le_bytes([e[10], e[11]]),
        serial: u32::from_le_bytes([e[12], e[13], e[14], e[15]]),
        week: e[16],
        year: 1990 + e[17] as u16,
        version: (e[18], e[19]),
        size_cm: (e[21], e[22]),
        ..Default::default()
    };
    for d in e[54..126].chunks(18) {
        if d[0] != 0 || d[1] != 0 {
            continue;
        }
        let text = || {
            let s: String = d[5..18]
                .iter()
                .take_while(|&&b| b != b'\n' && b != 0)
                .map(|&b| b as char)
                .collect();
            let s = s.trim().to_string();
            (!s.is_empty()).then_some(s)
        };
        match d[3] {
            0xfc => out.name = text(),
            0xff => out.serial_text = text(),
            _ => {}
        }
    }
    Some(out)
}

fn monitor(root: &Path, connector: &str, out: &mut Vec<Section>) {
    let path = root.join("sys/class/drm").join(connector).join("edid");
    let Some(e) = fs::read(&path).ok().and_then(|b| parse_edid(&b)) else {
        return;
    };
    let mut s = Builder::new();
    s.add("monitor_name", "Name", e.name.clone());
    s.add(
        "monitor_vendor",
        "Manufacturer",
        Some(match lookup(PNP_IDS, &e.vendor) {
            Some(n) => format!("{n} ({})", e.vendor),
            None => e.vendor.clone(),
        }),
    );
    s.add(
        "monitor_product",
        "Product code",
        Some(format!("{:04x}", e.product_code)),
    );
    let serial = match (&e.serial_text, e.serial) {
        (Some(t), _) => Some(t.clone()),
        (None, 0) => None,
        (None, n) => Some(n.to_string()),
    };
    s.add("monitor_serial", "Serial number", serial);
    s.add(
        "monitor_made",
        "Made",
        Some(match e.week {
            1..=53 => format!("week {} of {}", e.week, e.year),
            0xff => format!("model year {}", e.year),
            _ => e.year.to_string(),
        }),
    );
    if e.size_cm.0 > 0 && e.size_cm.1 > 0 {
        let (w, h) = (e.size_cm.0 as f64, e.size_cm.1 as f64);
        let inch = (w * w + h * h).sqrt() / 2.54;
        s.add(
            "monitor_size",
            "Size",
            Some(format!("{} x {} cm ({inch:.1}\")", e.size_cm.0, e.size_cm.1)),
        );
    }
    s.add(
        "monitor_edid",
        "EDID version",
        Some(format!("{}.{}", e.version.0, e.version.1)),
    );
    s.done("monitor", "Monitor", out);
}

/// The tablet's pen digitizer: a Wacom device on the same hub.
fn pen(root: &Path, interface: &str, out: &mut Vec<Section>) {
    let dev = interface.split(':').next().unwrap_or(interface);
    let Some((hub, _)) = dev.rsplit_once('.') else {
        return;
    };
    let devices = root.join("sys/bus/usb/devices");
    let Ok(entries) = fs::read_dir(&devices) else {
        return;
    };
    let mut names: Vec<String> = entries
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| !n.contains(':') && n.rsplit_once('.').is_some_and(|(h, _)| h == hub))
        .collect();
    names.sort();
    for n in names {
        let dir = devices.join(&n);
        if read(&dir.join("idVendor")).as_deref() != Some("056a") {
            continue;
        }
        let mut s = Builder::new();
        s.add("pen_product", "Product", read(&dir.join("product")));
        s.add("pen_manufacturer", "Manufacturer", read(&dir.join("manufacturer")));
        s.add("pen_serial", "Serial number", read(&dir.join("serial")));
        s.add("pen_usb_id", "USB ID", usb_id(&dir));
        s.add("pen_usb_path", "USB device", Some(n.clone()));
        s.add(
            "pen_release",
            "Device release",
            read(&dir.join("bcdDevice")).map(|b| bcd_text(&b)),
        );
        s.done("pen", "Pen digitizer", out);
        return;
    }
}

fn platform_text(p: &str) -> String {
    match p {
        "lite" => "Lite (48 MHz clock)".into(),
        "super-lite" => "Super Lite (40 MHz clock)".into(),
        p => p.into(),
    }
}

/// `16062899 17091401` from the driver: boot code and image code.
fn firmware_parts(fw: &str) -> (Option<String>, Option<String>) {
    let mut it = fw.split_whitespace().map(String::from);
    (it.next(), it.next())
}

/// All sections for a display; `info` is the driver's view when bound.
pub fn identify(display: &Display, info: Option<&Info>) -> Vec<Section> {
    let root = &display.root;
    let dir = usb_device_dir(root, &display.interface);
    let mut out = Vec::new();

    let mut s = Builder::new();
    s.add(
        "product_name",
        "Product",
        product(display, info).map(|p| p.full.to_string()),
    );
    s.add("manufacturer", "Manufacturer", read(&dir.join("manufacturer")));
    s.add(
        "usb_product",
        "Product string",
        read(&dir.join("product")).or_else(|| display.product.clone()),
    );
    s.add("board", "Board", info.and_then(|i| i.board.clone()));
    s.add("usb_serial", "Serial number", read(&dir.join("serial")));
    s.done("product", "Product", &mut out);

    let mut s = Builder::new();
    s.add(
        "chip",
        "Chip",
        Some(
            match display.chip {
                Chip::T6 => "MCT Trigger 6",
                Chip::T2 => "MCT Trigger II",
                Chip::Unknown => "Unknown",
            }
            .into(),
        ),
    );
    if let Some(i) = info {
        s.add("platform", "Platform", i.platform.as_deref().map(platform_text));
        let (boot, image) = i
            .firmware
            .as_deref()
            .map(firmware_parts)
            .unwrap_or_default();
        s.add("firmware_image", "Firmware", image);
        s.add("firmware_boot", "Boot code", boot);
        s.add("chip_serial", "Chip serial", i.serial.clone());
        s.add(
            "feature_level",
            "Feature level",
            i.feature_level.map(crate::text::feature_level),
        );
        s.add("vram_mb", "Video memory", i.vram_mb.map(crate::text::vram));
        s.add("display_flags", "Display flags", i.display_flags.clone());
    }
    s.done("chip", "Chip", &mut out);

    let mut s = Builder::new();
    s.add("usb_id", "USB ID", usb_id(&dir));
    s.add(
        "usb_release",
        "Device release",
        read(&dir.join("bcdDevice")).map(|b| bcd_text(&b)),
    );
    s.add(
        "usb_version",
        "USB version",
        read(&dir.join("version")),
    );
    s.add(
        "usb_speed",
        "Link",
        read(&dir.join("speed")).map(|v| speed_text(&v)),
    );
    let place = match (read(&dir.join("busnum")), read(&dir.join("devnum"))) {
        (Some(b), Some(d)) => Some(format!(
            "{} (bus {b}, device {d})",
            dir.file_name().unwrap_or_default().to_string_lossy()
        )),
        _ => None,
    };
    s.add("usb_device", "USB device", place);
    s.add("interface", "Interface", Some(display.interface.clone()));
    s.add(
        "usb_power",
        "Max power",
        read(&dir.join("bMaxPower")).map(|p| p.replace("mA", " mA")),
    );
    s.done("usb", "USB", &mut out);

    let mut s = Builder::new();
    s.add("driver", "In use", Some(display.route.label()));
    s.add(
        "driver_version",
        "Driver version",
        info.and_then(|i| i.driver_version.clone()),
    );
    s.add("connector", "Connector", info.and_then(|i| i.connector.clone()));
    s.add("backlight", "Backlight", info.and_then(|i| i.backlight.clone()));
    s.done("driver", "Driver", &mut out);

    if let Some(c) = info.and_then(|i| i.connector.as_deref()) {
        monitor(root, c, &mut out);
    }
    if display.chip == Chip::T2 {
        pen(root, &display.interface, &mut out);
    }
    out
}

/// All sections as aligned text, indented by `indent`.
pub fn sections_text(sections: &[Section], indent: &str) -> String {
    let width = sections
        .iter()
        .flat_map(|s| s.fields.iter().map(|f| f.label.chars().count()))
        .max()
        .unwrap_or(0)
        + 1;
    let mut out = String::new();
    for s in sections {
        out.push_str(&format!("{indent}{}\n", s.title));
        for f in &s.fields {
            out.push_str(&format!(
                "{indent}  {:width$}  {}\n",
                format!("{}:", f.label),
                f.value
            ));
        }
    }
    out
}

/// A report of the system and every display, for bug reports.
pub fn report(snaps: &[crate::Snapshot]) -> String {
    let root = crate::sysfs_root();
    let kernel = read(&root.join("proc/sys/kernel/osrelease")).unwrap_or_else(|| "?".into());
    let mut out = format!(
        "{} {}\nKernel {kernel}\nmct module: {}\n",
        crate::APP_NAME,
        crate::VERSION,
        if crate::usb::module_loaded(&root, "mct") {
            "loaded"
        } else {
            "not loaded"
        }
    );
    if snaps.is_empty() {
        out.push_str("\nNo MCT displays found.\n");
    }
    for s in snaps {
        out.push_str(&format!("\n{} ({})\n", s.name(), s.display.interface));
        out.push_str(&sections_text(&s.ident, "  "));
        for w in s.warnings() {
            out.push_str(&format!("  Warning: {}\n", w.text()));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bcd() {
        assert_eq!(bcd_text("1010"), "10.10");
        assert_eq!(bcd_text("0001"), "0.01");
        assert_eq!(bcd_text("0101"), "1.01");
    }

    #[test]
    fn edid() {
        let mut e = vec![0u8; 128];
        e[..8].copy_from_slice(&[0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0]);
        // "GSM"
        e[8] = 0x1e;
        e[9] = 0x6d;
        e[10] = 0x34;
        e[11] = 0x12;
        e[16] = 8;
        e[17] = 30;
        e[18] = 1;
        e[19] = 3;
        e[21] = 60;
        e[22] = 34;
        let d = &mut e[54..72];
        d[3] = 0xfc;
        d[5..11].copy_from_slice(b"27GN7\n");
        let p = parse_edid(&e).unwrap();
        assert_eq!(p.vendor, "GSM");
        assert_eq!(p.product_code, 0x1234);
        assert_eq!(p.name.as_deref(), Some("27GN7"));
        assert_eq!((p.week, p.year), (8, 2020));
        assert!(parse_edid(&e[..100]).is_none());
    }
}

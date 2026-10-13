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
    pub key: String,
    pub label: String,
    pub value: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Section {
    pub key: &'static str,
    pub title: &'static str,
    /// Technical detail: shown by `-vv` and collapsed in the window.
    pub detail: bool,
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

const DTU_1031X: Product = Product {
    short: "Wacom DTU-1031X",
    full: "Wacom DTU-1031X pen display",
};

/// Products known by their USB IDs and product string ("" matches any).
const USB_PRODUCTS: &[(&str, &str, &str, Product)] = &[
    (VENDOR, "520d", "DTU-1031X", DTU_1031X),
    (VENDOR, "5201", "DTU-1031X", DTU_1031X),
    (VENDOR, "520f", "DTU-1031X", DTU_1031X),
];

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

    fn add(&mut self, key: impl Into<String>, label: impl Into<String>, value: Option<String>) {
        if let Some(value) = value.filter(|v| !v.is_empty()) {
            self.fields.push(Field {
                key: key.into(),
                label: label.into(),
                value,
            });
        }
    }

    fn push(self, key: &'static str, title: &'static str, detail: bool, out: &mut Vec<Section>) {
        if !self.fields.is_empty() {
            out.push(Section {
                key,
                title,
                detail,
                fields: self.fields,
            });
        }
    }

    fn done(self, key: &'static str, title: &'static str, out: &mut Vec<Section>) {
        self.push(key, title, false, out)
    }

    fn done_detail(self, key: &'static str, title: &'static str, out: &mut Vec<Section>) {
        self.push(key, title, true, out)
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
/// The Wacom device on the same hub as a tablet's display: its USB device
/// name (e.g. `6-3.2`) and sysfs directory.
fn pen_device(root: &Path, interface: &str) -> Option<(String, PathBuf)> {
    let dev = interface.split(':').next().unwrap_or(interface);
    let (hub, _) = dev.rsplit_once('.')?;
    let devices = root.join("sys/bus/usb/devices");
    let mut names: Vec<String> = fs::read_dir(&devices)
        .ok()?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| !n.contains(':') && n.rsplit_once('.').is_some_and(|(h, _)| h == hub))
        .collect();
    names.sort();
    names.into_iter().find_map(|n| {
        let dir = devices.join(&n);
        (read(&dir.join("idVendor")).as_deref() == Some("056a")).then_some((n, dir))
    })
}

/// The serial number of a tablet's pen digitizer (the display function has
/// none).
pub fn pen_serial(root: &Path, interface: &str) -> Option<String> {
    read(&pen_device(root, interface)?.1.join("serial"))
}

fn pen(root: &Path, interface: &str, out: &mut Vec<Section>) {
    if let Some((n, dir)) = pen_device(root, interface) {
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
    details(display, info, &dir, &mut out);
    out
}

/// The technical sections (`detail` set): what `-vv` adds.
fn details(display: &Display, info: Option<&Info>, dir: &Path, out: &mut Vec<Section>) {
    let mct = display.device.as_ref().map(|d| d.dir().to_path_buf());
    let mct_file = |name: &str| -> Option<String> {
        let s = fs::read_to_string(mct.as_ref()?.join(name)).ok()?;
        let s = s.trim().to_string();
        (!s.is_empty() && s != "-").then_some(s)
    };

    // the adapter's own view (T6, mct_t6 0.27.0 and later)
    let mut s = Builder::new();
    if let Some(f) = mct_file("functions") {
        for line in f.lines() {
            if let Some((id, name)) = line.split_once(' ') {
                s.add(format!("function_{id}"), format!("Function {id}"), Some(name.into()));
            }
        }
    }
    if let Some(c) = mct_file("clock_limits") {
        let mhz: Vec<String> = c
            .split_whitespace()
            .filter_map(|v| v.parse::<f64>().ok())
            .map(|k| format!("{:.0} MHz", k / 1000.0))
            .collect();
        if let [all, wide] = mhz.as_slice() {
            s.add(
                "clock_limits",
                "Pixel clock limit",
                Some(format!("{all} ({wide} from 2560 pixels wide)")),
            );
        }
    }
    if let Some(m) = mct_file("device_modes") {
        let modes: Vec<(&str, f64)> = m
            .lines()
            .filter_map(|l| {
                let (mode, clock) = l.split_once(' ')?;
                Some((mode, clock.trim().parse::<f64>().ok()? / 1000.0))
            })
            .collect();
        if let Some((mode, clock)) = modes.iter().max_by(|a, b| a.1.total_cmp(&b.1)) {
            s.add(
                "device_modes_max",
                "Mode table",
                Some(format!(
                    "{} modes, up to {mode} ({clock:.2} MHz)",
                    modes.len()
                )),
            );
        }
        let list: Vec<&str> = modes.iter().map(|(m, _)| *m).collect();
        s.add("device_modes", "Table modes", Some(list.join(", ")));
    }
    s.done_detail("adapter", "Adapter internals", out);

    // USB descriptors
    let r = |f: &str| read(&dir.join(f));
    let mut s = Builder::new();
    let class = |c: Option<String>, sub: Option<String>, proto: Option<String>| {
        let c = c?;
        let name = match c.as_str() {
            "00" => "Defined per interface",
            "01" => "Audio",
            "03" => "HID",
            "08" => "Mass storage",
            "09" => "Hub",
            "0e" => "Video",
            "ef" => "Miscellaneous",
            "ff" => "Vendor specific",
            _ => "Class",
        };
        Some(format!(
            "{name} (0x{c}), subclass 0x{}, protocol 0x{}",
            sub.unwrap_or_else(|| "?".into()),
            proto.unwrap_or_else(|| "?".into())
        ))
    };
    s.add(
        "usb_class",
        "Device class",
        class(r("bDeviceClass"), r("bDeviceSubClass"), r("bDeviceProtocol")),
    );
    let intf = display.root.join("sys/bus/usb/devices").join(&display.interface);
    let ri = |f: &str| read(&intf.join(f));
    s.add(
        "usb_interface_class",
        "Interface class",
        class(ri("bInterfaceClass"), ri("bInterfaceSubClass"), ri("bInterfaceProtocol")),
    );
    if let (Some(v), Some(n)) = (r("bConfigurationValue"), r("bNumConfigurations")) {
        s.add("usb_config", "Configuration", Some(format!("{v} of {n}")));
    }
    s.add("usb_interfaces", "Interfaces", r("bNumInterfaces"));
    if let Some(a) = r("bmAttributes") {
        let bits = u8::from_str_radix(&a, 16).unwrap_or(0);
        let mut v = vec![if bits & 0x40 != 0 {
            "self-powered"
        } else {
            "bus-powered"
        }];
        if bits & 0x20 != 0 {
            v.push("remote wakeup");
        }
        s.add("usb_attributes", "Attributes", Some(format!("0x{a}: {}", v.join(", "))));
    }
    s.add("usb_ep0", "Control packet size", r("bMaxPacketSize0").map(|v| format!("{v} bytes")));
    if let (Some(rx), Some(tx)) = (r("rx_lanes"), r("tx_lanes")) {
        s.add("usb_lanes", "Lanes", Some(format!("{rx} rx, {tx} tx")));
    }
    let lpm: Vec<String> = ["u1", "u2"]
        .iter()
        .filter_map(|u| {
            read(&dir.join(format!("power/usb3_hardware_lpm_{u}")))
                .map(|v| format!("{} {v}", u.to_uppercase()))
        })
        .collect();
    if !lpm.is_empty() {
        s.add("usb_lpm", "Link power management", Some(lpm.join(", ")));
    }
    s.add("usb_ltm", "Latency tolerance messages", r("ltm_capable"));
    s.add("usb_power_control", "Runtime power control", read(&dir.join("power/control")));
    s.add("usb_runtime_status", "Runtime status", read(&dir.join("power/runtime_status")));
    s.add("usb_removable", "Removable", r("removable"));
    s.add("usb_quirks", "Quirks", r("quirks"));
    s.add("usb_urbs", "URBs since connect", r("urbnum"));
    let mut eps: Vec<PathBuf> = fs::read_dir(&intf)
        .map(|d| {
            d.flatten()
                .map(|e| e.path())
                .filter(|p| {
                    p.file_name()
                        .is_some_and(|n| n.to_string_lossy().starts_with("ep_"))
                })
                .collect()
        })
        .unwrap_or_default();
    eps.sort();
    for ep in eps {
        let e = |f: &str| read(&ep.join(f));
        let Some(addr) = e("bEndpointAddress") else {
            continue;
        };
        let size = e("wMaxPacketSize")
            .and_then(|v| u32::from_str_radix(&v, 16).ok())
            .map(|v| format!(", {v} bytes"))
            .unwrap_or_default();
        let interval = e("interval")
            .filter(|i| e("type").as_deref() == Some("Interrupt") || i != "0ms")
            .map(|i| format!(", every {i}"))
            .unwrap_or_default();
        s.add(
            format!("endpoint_{addr}"),
            format!("Endpoint 0x{addr}"),
            Some(format!(
                "{} {}{size}{interval}",
                e("type").unwrap_or_default(),
                e("direction").unwrap_or_default()
            )),
        );
    }
    s.done_detail("usb_descriptors", "USB descriptors", out);

    // the monitor's EDID in detail
    if let Some(c) = info.and_then(|i| i.connector.as_deref()) {
        let path = display.root.join("sys/class/drm").join(c).join("edid");
        if let Ok(e) = fs::read(&path) {
            let mut s = Builder::new();
            for (k, l, v) in edid_details(&e) {
                s.add(k, l, Some(v));
            }
            s.done_detail("monitor_details", "Monitor EDID", out);
        }
    }

    // driver
    let mut s = Builder::new();
    let names = |sub: &str| -> Vec<String> {
        let mut v: Vec<String> = fs::read_dir(intf.join(sub))
            .map(|d| {
                d.flatten()
                    .map(|e| e.file_name().to_string_lossy().into_owned())
                    .collect()
            })
            .unwrap_or_default();
        v.sort();
        v
    };
    let cards: Vec<String> = names("drm")
        .into_iter()
        .filter(|n| n.starts_with("card"))
        .collect();
    if !cards.is_empty() {
        s.add("drm_devices", "DRM devices", Some(cards.join(", ")));
    }
    let fbs = names("graphics");
    if !fbs.is_empty() {
        s.add("framebuffer", "Framebuffer", Some(fbs.join(", ")));
    }
    let params = display.root.join("sys/module/mct/parameters");
    let mut defaults: Vec<String> = fs::read_dir(&params)
        .map(|d| {
            d.flatten()
                .filter_map(|e| {
                    let n = e.file_name().to_string_lossy().into_owned();
                    Some(format!("{n}={}", read(&e.path())?))
                })
                .collect()
        })
        .unwrap_or_default();
    defaults.sort();
    if !defaults.is_empty() {
        s.add("module_defaults", "Module defaults", Some(defaults.join(", ")));
    }
    s.add(
        "module_srcversion",
        "Module srcversion",
        read(&display.root.join("sys/module/mct/srcversion")),
    );
    s.done_detail("driver_details", "Driver details", out);
}

/// Decodes what helps with mode problems: input, preferred timing, range
/// limits, HDMI clock, CTA modes; and the raw bytes.
pub fn edid_details(e: &[u8]) -> Vec<(String, String, String)> {
    let mut out: Vec<(String, String, String)> = Vec::new();
    let mut add = |k: &str, l: &str, v: String| out.push((k.into(), l.into(), v));
    if e.len() < 128 || parse_edid(e).is_none() {
        if !e.is_empty() {
            add("edid_invalid", "EDID", format!("{} bytes, not valid", e.len()));
        }
        return out;
    }
    let input = e[20];
    add(
        "edid_input",
        "Input",
        if input & 0x80 != 0 && e[19] < 4 {
            "digital".into()
        } else if input & 0x80 != 0 {
            // EDID 1.4: interface and bit depth
            let iface = match input & 0x0f {
                1 => "DVI",
                2 | 3 => "HDMI",
                5 => "DisplayPort",
                _ => "digital",
            };
            let depth = match (input >> 4) & 7 {
                1 => " 6 bpc",
                2 => " 8 bpc",
                3 => " 10 bpc",
                4 => " 12 bpc",
                _ => "",
            };
            format!("digital, {iface}{depth}")
        } else {
            "analog (VGA)".into()
        },
    );
    let mut have_preferred = false;
    for d in e[54..126].chunks(18) {
        let clock = u16::from_le_bytes([d[0], d[1]]) as u32 * 10;
        if clock != 0 {
            let ha = d[2] as u32 | ((d[4] as u32 & 0xf0) << 4);
            let hb = d[3] as u32 | ((d[4] as u32 & 0x0f) << 8);
            let va = d[5] as u32 | ((d[7] as u32 & 0xf0) << 4);
            let vb = d[6] as u32 | ((d[7] as u32 & 0x0f) << 8);
            let hz = clock as f64 * 1000.0 / ((ha + hb) as f64 * (va + vb) as f64);
            // the first detailed timing is the preferred one
            if !have_preferred {
                have_preferred = true;
                add(
                    "edid_preferred",
                    "Preferred timing",
                    format!("{ha}x{va} at {hz:.2} Hz, {:.2} MHz", clock as f64 / 1000.0),
                );
            }
        } else if d[3] == 0xfd {
            let f = d[4];
            let v = |b: u8, bit: u8| b as u32 + if f & bit != 0 { 255 } else { 0 };
            add(
                "edid_range",
                "Range limits",
                format!(
                    "{}-{} Hz vertical, {}-{} kHz horizontal, up to {} MHz",
                    v(d[5], 1),
                    v(d[6], 2),
                    v(d[7], 4),
                    v(d[8], 8),
                    d[9] as u32 * 10
                ),
            );
        }
    }
    add("edid_extensions", "Extension blocks", e[126].to_string());
    for block in e[128..].chunks(128).filter(|b| b.len() == 128 && b[0] == 0x02) {
        let end = (block[2] as usize).min(127);
        let mut i = 4;
        let mut vics = 0;
        while i < end {
            let tag = block[i] >> 5;
            let len = (block[i] & 0x1f) as usize;
            let p = &block[i + 1..(i + 1 + len).min(128)];
            match tag {
                2 => vics += p.len(),
                3 if p.len() >= 7 && p[..3] == [0x03, 0x0c, 0x00] => add(
                    "edid_hdmi",
                    "HDMI",
                    format!("HDMI 1.x, max TMDS clock {} MHz", p[6] as u32 * 5),
                ),
                3 if p.len() >= 5 && p[..3] == [0xd8, 0x5d, 0xc4] => add(
                    "edid_hdmi_forum",
                    "HDMI Forum",
                    format!("HDMI 2.x, max TMDS clock {} MHz", p[4] as u32 * 5),
                ),
                _ => {}
            }
            i += 1 + len;
        }
        if vics > 0 {
            add("edid_cta_modes", "CTA video modes", vics.to_string());
        }
    }
    let hex: Vec<String> = e
        .chunks(16)
        .map(|c| c.iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(" "))
        .collect();
    add("edid_hex", "Raw EDID", hex.join("\n"));
    out
}

/// All sections as aligned text, indented by `indent`.
pub fn sections_text(sections: &[Section], indent: &str) -> String {
    let width = sections
        .iter()
        .flat_map(|s| {
            s.fields
                .iter()
                .map(|f| f.label.chars().count())
        })
        .max()
        .unwrap_or(0)
        + 1;
    let mut out = String::new();
    for s in sections {
        out.push_str(&format!("{indent}{}\n", s.title));
        for f in &s.fields {
            // values over several lines (the raw EDID) continue under the first
            let mut lines = f.value.lines();
            out.push_str(&format!(
                "{indent}  {:width$}  {}\n",
                format!("{}:", f.label),
                lines.next().unwrap_or_default()
            ));
            for l in lines {
                out.push_str(&format!("{indent}  {:width$}  {l}\n", ""));
            }
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

        e[20] = 0x80;
        let d = edid_details(&e);
        let get = |k: &str| d.iter().find(|(key, ..)| key == k).map(|(.., v)| v.clone());
        assert_eq!(get("edid_input").as_deref(), Some("digital"));
        assert_eq!(get("edid_hex").map(|h| h.lines().count()), Some(8));
    }
}

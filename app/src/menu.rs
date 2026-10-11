// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! What the tray menu shows, independent of the tray library, so it can be
//! tested without a session bus.

use crate::device::{Chip, FORMATS, LOSSY_MAX};
use crate::text;
use crate::usb::Snapshot;

#[derive(Clone, Debug, PartialEq)]
pub enum Item {
    /// Greyed out device heading.
    Header(String),
    /// Greyed out line under a heading.
    Note(String),
    Separator,
    /// Radio items for `FORMATS`; `selected` indexes it.
    Formats {
        device: usize,
        selected: Option<usize>,
    },
    AllModes {
        device: usize,
        on: bool,
    },
    /// Radio items (label, brightness value).
    Brightness {
        device: usize,
        levels: Vec<(String, u32)>,
        selected: Option<usize>,
    },
    /// Radio items 0..=LOSSY_MAX.
    Lossy {
        device: usize,
        selected: Option<u32>,
    },
    /// Nothing connected.
    Empty,
}

/// Off and a few levels up to `max`: every level when there are few
/// (the DTU-1031X has 9), otherwise quarters.
pub fn brightness_levels(max: u32) -> Vec<(String, u32)> {
    let mut out = vec![("Off".to_string(), 0)];
    if max <= 10 {
        out.extend((1..=max).map(|v| (v.to_string(), v)));
        return out;
    }
    for pct in [25u64, 50, 75, 100] {
        let v = ((max as u64 * pct + 50) / 100) as u32;
        out.push((format!("{pct}%"), v));
    }
    out
}

/// Heading for a display: model and connector (or interface).
pub fn heading(snap: &Snapshot) -> String {
    format!("{}: {}", snap.name(), snap.place())
}

/// The display part of the tray menu; `errors[i]` is the last failed change
/// for display `i`, shown under its heading.
pub fn items(snaps: &[Snapshot], errors: &[Option<String>]) -> Vec<Item> {
    if snaps.is_empty() {
        return vec![Item::Empty];
    }
    let mut out = Vec::new();
    for (i, snap) in snaps.iter().enumerate() {
        if i > 0 {
            out.push(Item::Separator);
        }
        out.push(Item::Header(heading(snap)));
        let Some(info) = &snap.info else {
            // not on the kernel driver: no settings to offer
            out.push(Item::Note(snap.display.route.label()));
            if let Some(Some(e)) = errors.get(i) {
                out.push(Item::Note(e.clone()));
            }
            continue;
        };
        out.push(Item::Note(format!(
            "Mode: {}",
            info.mode.as_deref().unwrap_or("-")
        )));
        match info.chip {
            Chip::T6 => {
                out.push(Item::Note(format!("Format: {}", info.format_summary())));
            }
            Chip::T2 => {
                if let (Some(b), Some(m)) = (info.brightness, info.max_brightness) {
                    let s = if b == 0 {
                        "off".to_string()
                    } else {
                        format!("{b} of {m}")
                    };
                    out.push(Item::Note(format!("Brightness: {s}")));
                }
            }
            Chip::Unknown => {}
        }
        for w in info.warnings() {
            out.push(Item::Note(w.short()));
        }
        if let Some(Some(e)) = errors.get(i) {
            out.push(Item::Note(e.clone()));
        }
        if info.format.is_some() {
            let selected = info
                .format
                .as_deref()
                .and_then(|f| FORMATS.iter().position(|x| *x == f));
            out.push(Item::Formats {
                device: i,
                selected,
            });
        }
        if let Some(a) = info.all_modes {
            out.push(Item::AllModes {
                device: i,
                on: a != 0,
            });
        }
        if let (Some(b), Some(m)) = (info.brightness, info.max_brightness) {
            let levels = brightness_levels(m);
            let selected = levels.iter().position(|l| l.1 == b);
            out.push(Item::Brightness {
                device: i,
                levels,
                selected,
            });
        }
        if info.lossy.is_some() {
            out.push(Item::Lossy {
                device: i,
                selected: info.lossy.filter(|l| *l <= LOSSY_MAX),
            });
        }
    }
    out
}

/// Labels for the format radio items.
pub fn format_labels() -> Vec<String> {
    FORMATS
        .iter()
        .map(|f| text::format_label(f).to_string())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn levels() {
        let l = brightness_levels(255);
        assert_eq!(l[0], ("Off".into(), 0));
        assert_eq!(
            l.iter().map(|x| x.1).collect::<Vec<_>>(),
            [0, 64, 128, 191, 255]
        );
        let l = brightness_levels(9);
        assert_eq!(l.len(), 10);
        assert_eq!(l[9], ("9".into(), 9));
        assert_eq!(brightness_levels(0).len(), 1);
    }

    #[test]
    fn empty() {
        assert_eq!(items(&[], &[]), vec![Item::Empty]);
    }
}

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Wording shared by the CLI, the window and the tray.

pub fn format_label(format: &str) -> &'static str {
    match format {
        "auto" => "Automatic",
        "rgb24" => "RGB, 24 bit",
        "rgb32" => "RGB, 32 bit",
        "yuv444" => "YUV 4:4:4",
        "nv12" => "NV12",
        _ => "Unknown",
    }
}

pub fn format_description(format: &str) -> &'static str {
    match format {
        "auto" => "Exact colours when still, NV12 during motion",
        "rgb24" => "Exact colours",
        "rgb32" => "Exact colours, more USB traffic",
        "yuv444" => "Full colour detail, slight rounding",
        "nv12" => "Least USB traffic, half colour detail",
        _ => "",
    }
}

pub const ALL_MODES_DESCRIPTION: &str =
    "Include modes above the monitor's native resolution. The monitor is probed again.";

pub const BRIGHTNESS_DESCRIPTION: &str = "0 turns the backlight off";

pub const LOSSY_DESCRIPTION: &str =
    "Allow lossy updates during heavy motion. 0 keeps every update lossless.";

pub const HALF_MOTION_DESCRIPTION: &str = "Send full-screen motion at half resolution, upscaled by \
     the tablet: a little blur instead of streaks. The exact picture returns when motion stops.";

pub const CREDITS: &str = "Protocol work builds on cyrozap's notes and the trigger6 driver";

pub fn usb_link(speed: &str) -> String {
    match speed {
        "usb2" => "USB 2".into(),
        "usb3" => "USB 3".into(),
        s => s.into(),
    }
}

pub fn feature_level(level: i64) -> String {
    if level < 0 {
        "unknown".into()
    } else {
        level.to_string()
    }
}

pub fn vram(mb: u64) -> String {
    format!("{mb} MB")
}

pub fn pixel_format(p: &str) -> String {
    if p == "-" {
        "none".into()
    } else {
        p.into()
    }
}

/// Byte counts for people: 1.2 GB, 340.0 MB, 12 kB.
pub fn bytes(n: u64) -> String {
    let n = n as f64;
    if n >= 1e9 {
        format!("{:.1} GB", n / 1e9)
    } else if n >= 1e6 {
        format!("{:.1} MB", n / 1e6)
    } else if n >= 1e3 {
        format!("{:.0} kB", n / 1e3)
    } else {
        format!("{n} B")
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn bytes() {
        assert_eq!(super::bytes(999), "999 B");
        assert_eq!(super::bytes(3_500_000_000), "3.5 GB");
        assert_eq!(super::bytes(12_300_000), "12.3 MB");
    }
}

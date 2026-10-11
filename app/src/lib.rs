// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Access to mct devices through the driver's sysfs interface
//! (docs/driver/companion-app.md), shared by mctctl, the window and the tray.

pub mod device;
pub mod ident;
pub mod menu;
pub mod rates;
pub mod settings;
pub mod switch;
pub mod text;
pub mod usb;

#[doc(hidden)]
pub mod fake;

pub use device::{list_devices, sysfs_root, Chip, Device, Error, Info, Warning};
pub use rates::{RateMeter, Rates};
pub use usb::{discover, Display, Route, Snapshot};

pub const APP_ID: &str = "org.mctlinux.Manager";
pub const APP_NAME: &str = "MCT Display Manager";
pub const VERSION: &str = env!("CARGO_PKG_VERSION");
pub const AUTHOR: &str = "Filip Piękoś";
pub const AUTHOR_URL: &str = "https://github.com/XXCoreRangerX";
pub const WEBSITE: &str = "https://github.com/XXCoreRangerX/mct-linux";
pub const ISSUES: &str = "https://github.com/XXCoreRangerX/mct-linux/issues";
pub const DOCS: &str = "https://github.com/XXCoreRangerX/mct-linux/tree/main/docs";
pub const CHANGELOG: &str =
    "https://github.com/XXCoreRangerX/mct-linux/blob/main/CHANGELOG.md";

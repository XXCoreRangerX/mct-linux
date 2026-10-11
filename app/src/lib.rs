// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Access to mct devices through the driver's sysfs interface
//! (docs/companion-app.md), shared by mctctl, the window and the tray.

pub mod device;
pub mod menu;
pub mod rates;
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
/// Placeholder until the project has a public home.
pub const WEBSITE: &str = "https://example.org/mct-linux";

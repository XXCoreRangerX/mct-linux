// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Builds a fake sysfs tree with two T6 adapters and the T2 tablet, for
//! trying mctctl and mct-manager without a device:
//!
//!     cargo run --example fake_sysfs -- /tmp/mct-fake
//!     MCT_SYSFS_ROOT=/tmp/mct-fake cargo run --bin mct-manager

fn main() {
    let Some(dir) = std::env::args().nth(1) else {
        eprintln!("usage: fake_sysfs DIR");
        std::process::exit(2);
    };
    if let Err(e) = mct::fake::build(std::path::Path::new(&dir)) {
        eprintln!("fake_sysfs: {e}");
        std::process::exit(1);
    }
}

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! mct-manager: the MCT Display Manager window, and with --tray the tray icon.

mod page;
mod tray;
mod window;

use clap::Parser;
use relm4::RelmApp;

#[derive(Parser)]
#[command(
    name = "mct-manager",
    version,
    about = "Settings and status for MCT USB displays"
)]
struct Args {
    /// Start in the tray; the window opens from the tray menu and closing it
    /// keeps the tray running
    #[arg(long)]
    tray: bool,
}

fn main() {
    let args = Args::parse();
    let app = RelmApp::new(mct::APP_ID).visible_on_activate(!args.tray);
    // GApplication would reject our options; they are parsed above
    let argv0 = std::env::args()
        .next()
        .unwrap_or_else(|| "mct-manager".into());
    app.with_args(vec![argv0])
        .run::<window::App>(window::AppInit { tray: args.tray });
}

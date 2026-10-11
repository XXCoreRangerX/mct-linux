// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! The tray icon: a StatusNotifierItem whose menu (DBusMenu) the bar draws.
//! The menu's content comes from `mct::menu`; this file only maps it.

use std::path::PathBuf;
use std::thread;
use std::time::Duration;

use ksni::blocking::TrayMethods;
use ksni::menu::{CheckmarkItem, RadioGroup, RadioItem, StandardItem, SubMenu};
use ksni::MenuItem;
use mct::device::{FORMATS, LOSSY_MAX};
use mct::menu::{self, Item};
use mct::{discover, Device, Error, Info, Snapshot};

type Callback = Box<dyn Fn() + Send>;

struct Tray {
    snaps: Vec<Snapshot>,
    /// Last failed change per display, shown in the menu.
    errors: Vec<Option<String>>,
    open: Callback,
    quit: Callback,
}

fn snapshot() -> Vec<Snapshot> {
    discover().into_iter().map(Snapshot::take).collect()
}

/// What the menu shows, without the counters that change all the time.
fn menu_key(snaps: &[Snapshot]) -> Vec<Snapshot> {
    snaps
        .iter()
        .map(|s| Snapshot {
            display: s.display.clone(),
            info: s.info.as_ref().map(|i| Info {
                updates: None,
                bytes: None,
                ..i.clone()
            }),
        })
        .collect()
}

fn interfaces(snaps: &[Snapshot]) -> Vec<&str> {
    snaps.iter().map(|s| s.display.interface.as_str()).collect()
}

/// DBusMenu treats `_` as a mnemonic marker.
fn label(s: &str) -> String {
    s.replace('_', "__")
}

fn note(s: &str) -> MenuItem<Tray> {
    StandardItem {
        label: label(s),
        enabled: false,
        ..Default::default()
    }
    .into()
}

impl Tray {
    fn set_snaps(&mut self, snaps: Vec<Snapshot>) {
        if interfaces(&snaps) != interfaces(&self.snaps) {
            self.errors = vec![None; snaps.len()];
        }
        self.snaps = snaps;
    }

    fn reload(&mut self) {
        self.set_snaps(snapshot());
    }

    fn change(&mut self, device: usize, f: impl FnOnce(&Device) -> Result<(), Error>) {
        let Some(d) = self
            .snaps
            .get(device)
            .and_then(|s| s.display.device.clone())
        else {
            return;
        };
        let res = f(&d);
        if let Some(slot) = self.errors.get_mut(device) {
            *slot = res.err().map(|e| match e {
                Error::Permission { .. } => "Permission denied: see the window".to_string(),
                e => format!("Failed: {e}"),
            });
        }
        self.reload();
    }
}

impl ksni::Tray for Tray {
    fn id(&self) -> String {
        "mct-manager".into()
    }

    fn title(&self) -> String {
        mct::APP_NAME.into()
    }

    fn icon_name(&self) -> String {
        mct::APP_ID.into()
    }

    fn icon_theme_path(&self) -> String {
        // <prefix>/bin/mct-manager -> <prefix>/share/icons, so the icon is
        // found even when the prefix isn't in XDG_DATA_DIRS
        std::env::current_exe()
            .ok()
            .and_then(|p| Some(p.parent()?.parent()?.join("share/icons")))
            .filter(|p: &PathBuf| p.is_dir())
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    fn tool_tip(&self) -> ksni::ToolTip {
        let n = self.snaps.len();
        ksni::ToolTip {
            title: mct::APP_NAME.into(),
            description: match n {
                0 => "No MCT displays connected".into(),
                1 => "1 display".into(),
                n => format!("{n} displays"),
            },
            ..Default::default()
        }
    }

    fn activate(&mut self, _x: i32, _y: i32) {
        (self.open)();
    }

    fn menu_about_to_show(&mut self) {
        self.reload();
    }

    fn menu(&self) -> Vec<MenuItem<Self>> {
        let mut out: Vec<MenuItem<Self>> = Vec::new();
        for item in menu::items(&self.snaps, &self.errors) {
            out.push(match item {
                Item::Header(s) => note(&s),
                Item::Note(s) => note(&s),
                Item::Separator => MenuItem::Separator,
                Item::Empty => note("No MCT displays connected"),
                Item::Formats { device, selected } => SubMenu {
                    label: "Format".into(),
                    submenu: vec![RadioGroup {
                        selected: selected.unwrap_or(usize::MAX),
                        select: Box::new(move |t: &mut Self, i| {
                            if let Some(f) = FORMATS.get(i) {
                                t.change(device, |d| d.set_format(f));
                            }
                        }),
                        options: menu::format_labels()
                            .into_iter()
                            .map(|l| RadioItem {
                                label: label(&l),
                                ..Default::default()
                            })
                            .collect(),
                    }
                    .into()],
                    ..Default::default()
                }
                .into(),
                Item::AllModes { device, on } => CheckmarkItem {
                    label: "Show all available modes".into(),
                    checked: on,
                    activate: Box::new(move |t: &mut Self| {
                        t.change(device, |d| d.set_all_modes(!on))
                    }),
                    ..Default::default()
                }
                .into(),
                Item::Brightness {
                    device,
                    levels,
                    selected,
                } => SubMenu {
                    label: "Brightness".into(),
                    submenu: vec![RadioGroup {
                        selected: selected.unwrap_or(usize::MAX),
                        options: levels
                            .iter()
                            .map(|(l, _)| RadioItem {
                                label: label(l),
                                ..Default::default()
                            })
                            .collect(),
                        select: Box::new(move |t: &mut Self, i| {
                            if let Some(&(_, v)) = levels.get(i) {
                                t.change(device, |d| d.set_brightness(v));
                            }
                        }),
                    }
                    .into()],
                    ..Default::default()
                }
                .into(),
                Item::Lossy { device, selected } => SubMenu {
                    label: "Lossy".into(),
                    submenu: vec![RadioGroup {
                        selected: selected.map(|s| s as usize).unwrap_or(usize::MAX),
                        options: (0..=LOSSY_MAX)
                            .map(|l| RadioItem {
                                label: if l == 0 {
                                    "0 (lossless)".into()
                                } else {
                                    l.to_string()
                                },
                                ..Default::default()
                            })
                            .collect(),
                        select: Box::new(move |t: &mut Self, i| {
                            t.change(device, |d| d.set_lossy(i as u32))
                        }),
                    }
                    .into()],
                    ..Default::default()
                }
                .into(),
            });
        }
        out.push(MenuItem::Separator);
        out.push(
            StandardItem {
                label: "Open MCT Display Manager".into(),
                activate: Box::new(|t: &mut Self| (t.open)()),
                ..Default::default()
            }
            .into(),
        );
        out.push(
            StandardItem {
                label: "Quit".into(),
                icon_name: "application-exit".into(),
                activate: Box::new(|t: &mut Self| (t.quit)()),
                ..Default::default()
            }
            .into(),
        );
        out
    }
}

/// Starts the tray in its own thread; fails if there is no StatusNotifier
/// host (no bar with a tray).
pub fn spawn(
    open: impl Fn() + Send + 'static,
    quit: impl Fn() + Send + 'static,
) -> Result<(), ksni::Error> {
    let snaps = snapshot();
    let tray = Tray {
        errors: vec![None; snaps.len()],
        snaps,
        open: Box::new(open),
        quit: Box::new(quit),
    };
    let handle = tray.spawn()?;
    // hotplug and changes made elsewhere: update the menu when they show
    thread::spawn(move || {
        let mut last = Vec::new();
        while !handle.is_closed() {
            thread::sleep(Duration::from_secs(1));
            let snaps = snapshot();
            let key = menu_key(&snaps);
            if key != last {
                last = key;
                handle.update(|t: &mut Tray| t.set_snaps(snaps));
            }
        }
    });
    Ok(())
}

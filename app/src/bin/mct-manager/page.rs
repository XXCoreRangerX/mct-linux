// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! The page of one display: driver, display, settings, status, device.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;

use mct::device::{Mode, FORMATS, LOSSY_MAX, PERMISSION_HINT};
use mct::ident::Section;
use mct::{switch, text, Chip, Rates, Route, Snapshot};
use relm4::adw::prelude::*;
use relm4::gtk::glib;
use relm4::{adw, gtk, Sender};

use crate::window::AppMsg;

/// Which settings the user can write.
#[derive(Default)]
pub struct Writable {
    pub format: bool,
    pub all_modes: bool,
    pub lossy: bool,
    pub brightness: bool,
}

/// What the page shows.
pub struct State<'a> {
    pub snap: &'a Snapshot,
    pub rates: Option<Rates>,
    pub writable: &'a Writable,
    /// Why the legacy daemon can't be used, if it can't.
    pub legacy_reason: Option<&'static str>,
    pub switching: bool,
}

pub struct Page {
    pub view: adw::ToolbarView,
    banner: adw::Banner,
    driver_group: adw::PreferencesGroup,
    driver: adw::ActionRow,
    driver_button: gtk::Button,
    display_group: adw::PreferencesGroup,
    mode: adw::ActionRow,
    pixel_format: adw::ActionRow,
    /// The modes the connector offers, one row each.
    modes: adw::ExpanderRow,
    mode_rows: RefCell<(Vec<Mode>, Vec<adw::ActionRow>)>,
    settings: adw::PreferencesGroup,
    format: adw::ComboRow,
    all_modes: adw::SwitchRow,
    brightness_row: adw::ActionRow,
    brightness: gtk::Scale,
    lossy: adw::SpinRow,
    status_group: adw::PreferencesGroup,
    update_rate: adw::ActionRow,
    data_rate: adw::ActionRow,
    errors: adw::ActionRow,
    errors_icon: gtk::Image,
    recoveries: adw::ActionRow,
    recoveries_icon: gtk::Image,
    prefs: adw::PreferencesPage,
    /// Product, chip, USB, driver, monitor: rebuilt when they change.
    ident_groups: RefCell<Vec<adw::PreferencesGroup>>,
    /// What the identity groups were built for: sections and their field
    /// keys, and whether the driver row is left out. Values alone change
    /// in place, so open expanders and the scroll position stay.
    ident_shape: RefCell<(Vec<(&'static str, Vec<String>)>, bool)>,
    ident_rows: RefCell<HashMap<(&'static str, String), adw::ActionRow>>,
}

/// A row showing a value under its name.
fn property_row(title: &str) -> adw::ActionRow {
    let row = adw::ActionRow::builder()
        .title(title)
        .subtitle("-")
        .subtitle_selectable(true)
        .build();
    row.add_css_class("property");
    row
}

fn group(title: &str, rows: &[&adw::ActionRow]) -> adw::PreferencesGroup {
    let g = adw::PreferencesGroup::builder().title(title).build();
    for r in rows {
        g.add(*r);
    }
    g
}

fn warning_icon() -> gtk::Image {
    let i = gtk::Image::from_icon_name("dialog-warning-symbolic");
    i.add_css_class("warning");
    i.set_tooltip_text(Some("Since the device was connected"));
    i
}

/// Sets a row's value, hiding the row when there is none.
fn set(row: &adw::ActionRow, value: Option<String>) {
    row.set_visible(value.is_some());
    let v = value.unwrap_or_default();
    if row.subtitle().as_deref() != Some(v.as_str()) {
        row.set_subtitle(&v);
    }
}

fn capitalize(s: &str) -> String {
    let mut c = s.chars();
    match c.next() {
        Some(f) => f.to_uppercase().chain(c).collect(),
        None => String::new(),
    }
}

impl Page {
    /// Builds the page. Widget signals send `AppMsg`s unless `quiet` is set
    /// (while the page itself updates them).
    pub fn new(sender: &Sender<AppMsg>, quiet: &Rc<Cell<bool>>) -> Page {
        let send = |make: fn(f64) -> AppMsg| {
            let (s, q) = (sender.clone(), quiet.clone());
            move |v: f64| {
                if !q.get() {
                    s.emit(make(v))
                }
            }
        };

        let driver_button = gtk::Button::builder().valign(gtk::Align::Center).build();
        {
            let s = sender.clone();
            driver_button.connect_clicked(move |_| s.emit(AppMsg::SwitchDriver));
        }
        let driver = property_row("In use");
        driver.add_suffix(&driver_button);
        let driver_group = group("Driver", &[&driver]);

        let mode = property_row("Mode");
        let pixel_format = property_row("Pixel format");
        let display_group = group("Display", &[&mode, &pixel_format]);
        let modes = adw::ExpanderRow::builder().title("Modes").build();
        display_group.add(&modes);

        let format = adw::ComboRow::builder()
            .title("Format")
            .model(&gtk::StringList::new(&FORMATS.map(text::format_label)))
            .build();
        let f = send(|v| AppMsg::SetFormat(v as usize));
        format.connect_selected_notify(move |r| f(r.selected() as f64));
        let all_modes = adw::SwitchRow::builder()
            .title("Show all available modes")
            .subtitle(text::ALL_MODES_DESCRIPTION)
            .build();
        let f = send(|v| AppMsg::SetAllModes(v != 0.0));
        all_modes.connect_active_notify(move |r| f(if r.is_active() { 1.0 } else { 0.0 }));
        let brightness = gtk::Scale::with_range(gtk::Orientation::Horizontal, 0.0, 1.0, 1.0);
        brightness.set_digits(0);
        brightness.set_round_digits(0);
        brightness.set_draw_value(true);
        brightness.set_value_pos(gtk::PositionType::Left);
        brightness.set_width_request(220);
        brightness.set_valign(gtk::Align::Center);
        let f = send(|v| AppMsg::SetBrightness(v.round() as u32));
        brightness.connect_value_changed(move |r| f(r.value()));
        let brightness_row = adw::ActionRow::builder()
            .title("Brightness")
            .subtitle(text::BRIGHTNESS_DESCRIPTION)
            .build();
        brightness_row.add_suffix(&brightness);
        let lossy = adw::SpinRow::with_range(0.0, LOSSY_MAX as f64, 1.0);
        lossy.set_title("Lossy updates");
        lossy.set_subtitle(text::LOSSY_DESCRIPTION);
        let f = send(|v| AppMsg::SetLossy(v.round() as u32));
        lossy.connect_value_notify(move |r| f(r.value()));
        let settings = adw::PreferencesGroup::builder().title("Settings").build();
        settings.add(&format);
        settings.add(&all_modes);
        settings.add(&brightness_row);
        settings.add(&lossy);

        let update_rate = property_row("Updates per second");
        let data_rate = property_row("Data rate");
        let errors = property_row("Transfer errors");
        let errors_icon = warning_icon();
        errors.add_suffix(&errors_icon);
        let recoveries = property_row("Device restarts");
        let recoveries_icon = warning_icon();
        recoveries.add_suffix(&recoveries_icon);
        let status_group = group("Status", &[&update_rate, &data_rate, &errors, &recoveries]);

        let prefs = adw::PreferencesPage::new();
        prefs.add(&driver_group);
        prefs.add(&display_group);
        prefs.add(&settings);
        prefs.add(&status_group);

        let banner = adw::Banner::new("");
        let view = adw::ToolbarView::new();
        view.add_top_bar(&adw::HeaderBar::new());
        view.add_top_bar(&banner);
        view.set_content(Some(&prefs));

        Page {
            view,
            banner,
            driver_group,
            driver,
            driver_button,
            display_group,
            mode,
            pixel_format,
            modes,
            mode_rows: RefCell::new((Vec::new(), Vec::new())),
            settings,
            format,
            all_modes,
            brightness_row,
            brightness,
            lossy,
            status_group,
            update_rate,
            data_rate,
            errors,
            errors_icon,
            recoveries,
            recoveries_icon,
            prefs,
            ident_groups: RefCell::new(Vec::new()),
            ident_shape: RefCell::new((Vec::new(), false)),
            ident_rows: RefCell::new(HashMap::new()),
        }
    }

    /// One group per identity section. The driver in use is left out while
    /// the Driver group above shows it.
    fn show_ident(&self, sections: &[Section]) {
        let skip_route = self.driver_group.is_visible();
        let shown = |f: &mct::ident::Field| !(skip_route && f.key == "driver");
        let shape: Vec<(&'static str, Vec<String>)> = sections
            .iter()
            .map(|s| {
                let keys = s.fields.iter().filter(|f| shown(f)).map(|f| f.key.clone());
                (s.key, keys.collect())
            })
            .collect();

        // the same rows as before: only the values change
        if *self.ident_shape.borrow() == (shape.clone(), skip_route) {
            let rows = self.ident_rows.borrow();
            for s in sections {
                for f in s.fields.iter().filter(|f| shown(f)) {
                    if let Some(row) = rows.get(&(s.key, f.key.clone())) {
                        if row.subtitle().as_deref() != Some(f.value.as_str()) {
                            row.set_subtitle(&f.value);
                        }
                    }
                }
            }
            return;
        }

        *self.ident_shape.borrow_mut() = (shape, skip_route);
        for g in self.ident_groups.borrow_mut().drain(..) {
            self.prefs.remove(&g);
        }
        let mut rows = self.ident_rows.borrow_mut();
        rows.clear();
        let mut row = |section: &'static str, f: &mct::ident::Field| {
            let row = property_row(&f.label);
            row.set_subtitle(&f.value);
            if f.key == "edid_hex" {
                row.add_css_class("monospace");
            }
            rows.insert((section, f.key.clone()), row.clone());
            row
        };
        for s in sections.iter().filter(|s| !s.detail) {
            let group_rows: Vec<adw::ActionRow> = s
                .fields
                .iter()
                .filter(|f| shown(f))
                .map(|f| row(s.key, f))
                .collect();
            if group_rows.is_empty() {
                continue;
            }
            let g = group(s.title, &group_rows.iter().collect::<Vec<_>>());
            self.prefs.add(&g);
            self.ident_groups.borrow_mut().push(g);
        }
        // the technical sections, collapsed
        let details: Vec<&Section> = sections.iter().filter(|s| s.detail).collect();
        if !details.is_empty() {
            let g = adw::PreferencesGroup::builder()
                .title("Technical details")
                .build();
            for s in details {
                let expander = adw::ExpanderRow::builder().title(s.title).build();
                for f in &s.fields {
                    expander.add_row(&row(s.key, f));
                }
                g.add(&expander);
            }
            self.prefs.add(&g);
            self.ident_groups.borrow_mut().push(g);
        }
    }

    /// The mode list; rows are rebuilt only when the list changes, so the
    /// expander stays open.
    fn show_modes(&self, modes: &[Mode]) {
        self.modes.set_visible(!modes.is_empty());
        let mut shown = self.mode_rows.borrow_mut();
        if shown.0 == modes {
            return;
        }
        for r in shown.1.drain(..) {
            self.modes.remove(&r);
        }
        let preferred = modes.iter().find(|m| m.preferred).map(|m| m.name.as_str());
        let mut subtitle = format!("{} offered", modes.len());
        if let Some(p) = preferred {
            subtitle.push_str(&format!(", preferred {p}"));
        }
        self.modes.set_subtitle(&subtitle);
        for m in modes {
            let row = property_row(&m.name);
            let mut detail = format!("{:.2} MHz pixel clock", m.clock_khz as f64 / 1000.0);
            if m.preferred {
                detail.push_str(", the monitor's preferred mode");
            }
            row.set_subtitle(&detail);
            self.modes.add_row(&row);
            shown.1.push(row);
        }
        shown.0 = modes.to_vec();
    }

    pub fn show(&self, s: &State) {
        let info = s.snap.info.as_ref();

        // a setup problem (USB 2) stays visible above the page
        match s.snap.warnings().into_iter().find(|w| w.is_serious()) {
            Some(w) => {
                self.banner.set_title(&glib::markup_escape_text(&w.text()));
                self.banner.set_revealed(true);
            }
            None => self.banner.set_revealed(false),
        }

        self.show_driver(s);
        self.show_ident(&s.snap.ident);

        // the rest needs the kernel driver
        self.display_group.set_visible(info.is_some());
        self.settings.set_visible(info.is_some());
        self.status_group.set_visible(info.is_some());
        let Some(info) = info else { return };

        set(
            &self.mode,
            Some(info.mode.clone().unwrap_or_else(|| "-".into())),
        );
        set(
            &self.pixel_format,
            info.pixel_format.as_deref().map(|p| match (info.chip, p) {
                // the T2 always sends RGB24, run-length encoded per colour
                (Chip::T2, "RGB24") => "RGB24, RLE compressed".to_string(),
                _ => text::pixel_format(p),
            }),
        );
        self.show_modes(&info.modes);

        let (ups, mbs) = match s.rates {
            Some(r) => (
                format!("{:.1}", r.updates_per_s),
                format!("{:.2} MB/s", r.mb_per_s),
            ),
            None => ("-".into(), "-".into()),
        };
        set(&self.update_rate, Some(ups));
        set(&self.data_rate, Some(mbs));
        set(&self.errors, Some(info.errors.unwrap_or(0).to_string()));
        set(&self.recoveries, info.recoveries.map(|r| r.to_string()));
        self.errors_icon.set_visible(info.errors.unwrap_or(0) > 0);
        self.recoveries_icon
            .set_visible(info.recoveries.unwrap_or(0) > 0);

        self.show_settings(s);
    }

    fn show_driver(&self, s: &State) {
        let display = &s.snap.display;
        // a T6 on the kernel driver has nothing to switch: just a row below
        let quiet = display.route == Route::Kernel && !display.is_tablet();
        self.driver_group.set_visible(!quiet);
        set(&self.driver, Some(display.route.label()));
        let (label, target_legacy) = match display.route {
            Route::Kernel => ("Use legacy daemon", true),
            _ => ("Use kernel driver", false),
        };
        let offered = !target_legacy || display.is_tablet();
        self.driver_button.set_visible(offered);
        self.driver_button
            .set_label(if s.switching { "Switching..." } else { label });
        let reason = if target_legacy { s.legacy_reason } else { None };
        self.driver_button
            .set_sensitive(!s.switching && reason.is_none());
        self.driver_button
            .set_tooltip_text(reason.map(capitalize).as_deref());

        let mut notes: Vec<String> = Vec::new();
        if let Some(h) = display.route.hint() {
            notes.push(h.to_string());
        }
        if display.is_tablet() {
            notes.push(switch::LEGACY_NOTE.to_string());
        }
        if let (true, Some(r)) = (offered, reason) {
            notes.push(format!("{}.", capitalize(r)));
        }
        let d = notes.join(" ");
        if self.driver_group.description().as_deref().unwrap_or("") != d {
            self.driver_group
                .set_description(Some(d.as_str()).filter(|d| !d.is_empty()));
        }
    }

    fn show_settings(&self, s: &State) {
        let Some(info) = &s.snap.info else { return };
        let t6 = info.chip == Chip::T6;
        let t2 = info.chip == Chip::T2;

        self.format.set_visible(t6 && info.format.is_some());
        if let Some(i) = info
            .format
            .as_deref()
            .and_then(|f| FORMATS.iter().position(|x| *x == f))
        {
            if self.format.selected() != i as u32 {
                self.format.set_selected(i as u32);
            }
            self.format
                .set_subtitle(text::format_description(FORMATS[i]));
        }
        self.format.set_sensitive(s.writable.format);

        self.all_modes.set_visible(t6 && info.all_modes.is_some());
        let on = info.all_modes.unwrap_or(0) != 0;
        if self.all_modes.is_active() != on {
            self.all_modes.set_active(on);
        }
        self.all_modes.set_sensitive(s.writable.all_modes);

        let backlight = match (info.brightness, info.max_brightness) {
            (Some(b), Some(m)) if t2 => Some((b, m)),
            _ => None,
        };
        self.brightness_row.set_visible(backlight.is_some());
        if let Some((b, m)) = backlight {
            if self.brightness.adjustment().upper() != m as f64 {
                self.brightness.set_range(0.0, m as f64);
            }
            if self.brightness.value() != b as f64 {
                self.brightness.set_value(b as f64);
            }
        }
        self.brightness_row.set_sensitive(s.writable.brightness);

        self.lossy.set_visible(t2 && info.lossy.is_some());
        if let Some(l) = info.lossy {
            if self.lossy.value() != l as f64 {
                self.lossy.set_value(l as f64);
            }
        }
        self.lossy.set_sensitive(s.writable.lossy);

        self.settings.set_visible(
            self.format.is_visible()
                || self.all_modes.is_visible()
                || self.brightness_row.is_visible()
                || self.lossy.is_visible(),
        );

        // settings the user can't change: say how to fix it
        let read_only = (t6 && !(s.writable.format && s.writable.all_modes))
            || (t2 && !(s.writable.lossy && (s.writable.brightness || backlight.is_none())));
        let d = if read_only {
            format!("Read-only for your user. {PERMISSION_HINT}")
        } else {
            "Remembered for this display and restored when it reconnects.".to_string()
        };
        if self.settings.description().as_deref().unwrap_or("") != d {
            self.settings
                .set_description(Some(d.as_str()).filter(|d| !d.is_empty()));
        }
    }
}

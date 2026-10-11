// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! The main window: a sidebar of displays and the selected display's page.

use std::cell::Cell;
use std::rc::Rc;
use std::thread;
use std::time::{Duration, Instant};

use mct::device::FORMATS;
use mct::{discover, switch, Error, RateMeter, Rates, Route, Snapshot};
use relm4::adw::prelude::*;
use relm4::gtk::{gio, glib};
use relm4::{adw, gtk, ComponentParts, ComponentSender, SimpleComponent};

use crate::page::{self, Page, Writable};
use crate::tray;

#[derive(Debug)]
pub enum AppMsg {
    Tick,
    Select(usize),
    SetFormat(usize),
    SetAllModes(bool),
    SetBrightness(u32),
    SetLossy(u32),
    SwitchDriver,
    SwitchDone(Result<(), String>),
    About,
    /// The application was activated (started again, or the tray's "Open").
    Activated,
    Present,
    Quit,
}

pub struct AppInit {
    pub tray: bool,
}

pub struct App {
    window: adw::ApplicationWindow,
    tray_mode: bool,
    activations: u32,
    snaps: Vec<Snapshot>,
    /// Bumped when the set of displays changes, so the sidebar is rebuilt.
    list_version: u64,
    selected: Option<String>,
    show_content: bool,
    meter: RateMeter,
    rates: Option<Rates>,
    writable: Writable,
    legacy_reason: Option<&'static str>,
    switching: bool,
    /// Last failed action, shown as a toast; the serial tells new from shown.
    toast: (u64, String),
}

pub struct AppWidgets {
    stack: gtk::Stack,
    split: adw::NavigationSplitView,
    sidebar: gtk::ListBox,
    sidebar_rows: Vec<adw::ActionRow>,
    shown_version: u64,
    /// Set while the view updates widgets, so their signals are ignored.
    quiet: Rc<Cell<bool>>,
    content: adw::NavigationPage,
    page: Page,
    toasts: adw::ToastOverlay,
    shown_toast: u64,
}

fn primary_menu() -> gtk::MenuButton {
    let menu = gio::Menu::new();
    menu.append(Some("_About MCT Display Manager"), Some("win.about"));
    menu.append(Some("_Quit"), Some("win.quit"));
    gtk::MenuButton::builder()
        .icon_name("open-menu-symbolic")
        .tooltip_text("Main Menu")
        .primary(true)
        .menu_model(&menu)
        .build()
}

/// An error as one sentence for a toast.
fn message(e: &Error) -> String {
    let s = match e {
        Error::Permission { .. } => "Permission denied".to_string(),
        e => e.to_string(),
    };
    let mut c = s.chars();
    match c.next() {
        Some(f) => f.to_uppercase().chain(c).collect(),
        None => s,
    }
}

impl App {
    fn current(&self) -> Option<&Snapshot> {
        let sel = self.selected.as_deref()?;
        self.snaps.iter().find(|s| s.display.interface == sel)
    }

    fn rescan(&mut self) {
        let snaps: Vec<Snapshot> = discover().into_iter().map(Snapshot::take).collect();
        let key = |v: &[Snapshot]| -> Vec<String> {
            v.iter().map(|s| s.display.interface.clone()).collect()
        };
        if key(&snaps) != key(&self.snaps) {
            self.list_version += 1;
        }
        self.snaps = snaps;
        if self.current().is_none() {
            let first = self.snaps.first().map(|s| s.display.interface.clone());
            self.select(first);
        }
    }

    fn select(&mut self, interface: Option<String>) {
        if self.selected != interface {
            self.selected = interface;
            self.meter.reset();
            self.rates = None;
        }
    }

    /// Re-reads the shown display after a change, without a rate sample.
    fn reload_current(&mut self) {
        let Some(sel) = self.selected.clone() else {
            return;
        };
        if let Some(s) = self.snaps.iter_mut().find(|s| s.display.interface == sel) {
            *s = Snapshot::take(s.display.clone());
        }
    }

    /// Rates and what can be changed, for the shown display.
    fn sample(&mut self) {
        let info = self.current().and_then(|s| s.info.clone());
        match info {
            Some(i) => {
                let r =
                    self.meter
                        .sample(Instant::now(), i.updates.unwrap_or(0), i.bytes.unwrap_or(0));
                if r.is_some() {
                    self.rates = r;
                }
            }
            None => self.rates = None,
        }
        let (writable, reason) = match self.current() {
            Some(s) => {
                let w = match &s.display.device {
                    Some(d) => Writable {
                        format: d.can_write("format"),
                        all_modes: d.can_write("all_modes"),
                        lossy: d.can_write("lossy"),
                        brightness: d.can_write("brightness"),
                    },
                    None => Writable::default(),
                };
                let reason = if s.display.is_tablet() && s.display.route == Route::Kernel {
                    switch::legacy_unavailable(&s.display)
                } else {
                    None
                };
                (w, reason)
            }
            None => (Writable::default(), None),
        };
        self.writable = writable;
        self.legacy_reason = reason;
    }

    fn change(&mut self, f: impl FnOnce(&mct::Device) -> Result<(), Error>) {
        let Some(dev) = self.current().and_then(|s| s.display.device.clone()) else {
            return;
        };
        if let Err(e) = f(&dev) {
            self.toast(message(&e));
        }
        self.reload_current();
    }

    fn toast(&mut self, text: String) {
        self.toast = (self.toast.0 + 1, text);
    }

    fn present(&mut self) {
        self.rescan();
        self.sample();
        self.window.present();
    }

    fn switch_driver(&mut self, sender: &ComponentSender<Self>) {
        let Some(display) = self.current().map(|s| s.display.clone()) else {
            return;
        };
        if self.switching {
            return;
        }
        self.switching = true;
        let s = sender.input_sender().clone();
        // pkexec waits for the password dialog: not on the main loop
        thread::spawn(move || {
            let res = match display.route {
                Route::Kernel => switch::to_legacy(&display),
                _ => switch::to_kernel(&display),
            };
            s.emit(AppMsg::SwitchDone(res.map_err(|e| message(&e))));
        });
    }
}

impl SimpleComponent for App {
    type Input = AppMsg;
    type Output = ();
    type Init = AppInit;
    type Root = adw::ApplicationWindow;
    type Widgets = AppWidgets;

    fn init_root() -> Self::Root {
        adw::ApplicationWindow::builder()
            .title(mct::APP_NAME)
            .icon_name(mct::APP_ID)
            .default_width(860)
            .default_height(720)
            .width_request(360)
            .height_request(300)
            .build()
    }

    fn init(
        init: Self::Init,
        window: Self::Root,
        sender: ComponentSender<Self>,
    ) -> ComponentParts<Self> {
        let about = gio::SimpleAction::new("about", None);
        let s = sender.clone();
        about.connect_activate(move |_, _| s.input(AppMsg::About));
        window.add_action(&about);
        let quit = gio::SimpleAction::new("quit", None);
        let s = sender.clone();
        quit.connect_activate(move |_, _| s.input(AppMsg::Quit));
        window.add_action(&quit);

        let app = relm4::main_application();
        app.set_accels_for_action("win.quit", &["<Control>q"]);
        app.set_accels_for_action("window.close", &["<Control>w"]);
        let s = sender.clone();
        app.connect_activate(move |_| s.input(AppMsg::Activated));
        if init.tray {
            // the tray keeps running while the window is closed
            window.set_hide_on_close(true);
        }

        #[cfg(debug_assertions)]
        if let Some(display) = gtk::gdk::Display::default() {
            // icons from the source tree when run with cargo run
            gtk::IconTheme::for_display(&display)
                .add_search_path(concat!(env!("CARGO_MANIFEST_DIR"), "/data/icons"));
        }

        // nothing connected
        let empty = adw::ToolbarView::new();
        let header = adw::HeaderBar::new();
        header.pack_end(&primary_menu());
        empty.add_top_bar(&header);
        empty.set_content(Some(
            &adw::StatusPage::builder()
                .icon_name("video-display-symbolic")
                .title("No MCT displays connected")
                .description("Connect a Trigger 6 adapter or the DTU-1031X")
                .build(),
        ));

        // sidebar
        let quiet = Rc::new(Cell::new(false));
        let sidebar = gtk::ListBox::builder()
            .selection_mode(gtk::SelectionMode::Single)
            .build();
        sidebar.add_css_class("navigation-sidebar");
        let (s, q) = (sender.clone(), quiet.clone());
        sidebar.connect_row_selected(move |_, row| {
            if let (false, Some(row)) = (q.get(), row) {
                s.input(AppMsg::Select(row.index() as usize));
            }
        });
        let sidebar_view = adw::ToolbarView::new();
        let header = adw::HeaderBar::new();
        header.pack_end(&primary_menu());
        sidebar_view.add_top_bar(&header);
        sidebar_view.set_content(Some(
            &gtk::ScrolledWindow::builder()
                .hscrollbar_policy(gtk::PolicyType::Never)
                .child(&sidebar)
                .build(),
        ));

        let page = Page::new(sender.input_sender(), &quiet);
        let content = adw::NavigationPage::new(&page.view, "Display");
        let split = adw::NavigationSplitView::new();
        split.set_sidebar(Some(&adw::NavigationPage::new(&sidebar_view, "Displays")));
        split.set_content(Some(&content));
        split.set_min_sidebar_width(220.0);

        let stack = gtk::Stack::new();
        stack.add_named(&empty, Some("empty"));
        stack.add_named(&split, Some("displays"));
        let toasts = adw::ToastOverlay::new();
        toasts.set_child(Some(&stack));
        window.set_content(Some(&toasts));

        // narrow windows show one pane at a time
        let bp = adw::Breakpoint::new(
            adw::BreakpointCondition::parse("max-width: 560sp").expect("valid condition"),
        );
        bp.add_setter(&split, "collapsed", Some(&true.to_value()));
        window.add_breakpoint(bp);

        let mut model = App {
            window: window.clone(),
            tray_mode: false,
            activations: 0,
            snaps: Vec::new(),
            list_version: 0,
            selected: None,
            show_content: false,
            meter: RateMeter::new(),
            rates: None,
            writable: Writable::default(),
            legacy_reason: None,
            switching: false,
            toast: (0, String::new()),
        };
        model.rescan();
        model.sample();

        if init.tray {
            let (open, quit) = (sender.input_sender().clone(), sender.input_sender().clone());
            match tray::spawn(
                move || open.emit(AppMsg::Present),
                move || quit.emit(AppMsg::Quit),
            ) {
                Ok(()) => model.tray_mode = true,
                Err(e) => {
                    eprintln!("mct-manager: no system tray available ({e}), opening the window");
                    sender.input(AppMsg::Present);
                }
            }
        }

        let s = sender.clone();
        glib::timeout_add_local(Duration::from_secs(1), move || {
            s.input(AppMsg::Tick);
            glib::ControlFlow::Continue
        });

        let widgets = AppWidgets {
            stack,
            split,
            sidebar,
            sidebar_rows: Vec::new(),
            shown_version: u64::MAX,
            quiet,
            content,
            page,
            toasts,
            shown_toast: 0,
        };
        ComponentParts { model, widgets }
    }

    fn update(&mut self, msg: AppMsg, sender: ComponentSender<Self>) {
        self.show_content = false;
        match msg {
            AppMsg::Tick => {
                if self.window.is_visible() {
                    self.rescan();
                    self.sample();
                } else {
                    // nothing shown; start the rates afresh when shown again
                    self.meter.reset();
                    self.rates = None;
                }
            }
            AppMsg::Select(i) => {
                if let Some(s) = self.snaps.get(i) {
                    self.select(Some(s.display.interface.clone()));
                    self.sample();
                    self.show_content = true;
                }
            }
            AppMsg::SetFormat(i) => {
                let Some(fmt) = FORMATS.get(i) else { return };
                let cur = self
                    .current()
                    .and_then(|s| s.info.as_ref())
                    .and_then(|i| i.format.clone());
                if cur.as_deref() != Some(fmt) {
                    self.change(|d| d.set_format(fmt));
                }
            }
            AppMsg::SetAllModes(on) => {
                let cur = self
                    .current()
                    .and_then(|s| s.info.as_ref())
                    .and_then(|i| i.all_modes);
                if cur.map(|a| a != 0) != Some(on) {
                    self.change(|d| d.set_all_modes(on));
                }
            }
            AppMsg::SetBrightness(v) => {
                let cur = self
                    .current()
                    .and_then(|s| s.info.as_ref())
                    .and_then(|i| i.brightness);
                if cur != Some(v) {
                    self.change(|d| d.set_brightness(v));
                }
            }
            AppMsg::SetLossy(v) => {
                let cur = self
                    .current()
                    .and_then(|s| s.info.as_ref())
                    .and_then(|i| i.lossy);
                if cur != Some(v) {
                    self.change(|d| d.set_lossy(v));
                }
            }
            AppMsg::SwitchDriver => self.switch_driver(&sender),
            AppMsg::SwitchDone(res) => {
                self.switching = false;
                if let Err(e) = res {
                    self.toast(e);
                }
                self.rescan();
                self.sample();
            }
            AppMsg::About => about(&self.window),
            AppMsg::Activated => {
                self.activations += 1;
                // in tray mode the first activation is the start itself
                if !(self.tray_mode && self.activations == 1) {
                    self.present();
                }
            }
            AppMsg::Present => self.present(),
            AppMsg::Quit => relm4::main_application().quit(),
        }
    }

    fn update_view(&self, w: &mut AppWidgets, _sender: ComponentSender<Self>) {
        w.quiet.set(true);
        self.show(w);
        w.quiet.set(false);
    }
}

impl App {
    fn show(&self, w: &mut AppWidgets) {
        w.stack.set_visible_child_name(if self.snaps.is_empty() {
            "empty"
        } else {
            "displays"
        });

        if w.shown_toast != self.toast.0 {
            w.shown_toast = self.toast.0;
            w.toasts
                .add_toast(adw::Toast::new(&glib::markup_escape_text(&self.toast.1)));
        }

        if w.shown_version != self.list_version {
            w.sidebar.remove_all();
            w.sidebar_rows.clear();
            for _ in &self.snaps {
                let row = adw::ActionRow::new();
                w.sidebar.append(&row);
                w.sidebar_rows.push(row);
            }
            w.shown_version = self.list_version;
        }
        for (row, snap) in w.sidebar_rows.iter().zip(&self.snaps) {
            let title = glib::markup_escape_text(snap.name()).to_string();
            let subtitle = match &snap.info {
                Some(i) => format!("{}, {}", snap.place(), i.mode.as_deref().unwrap_or("-")),
                None => snap.display.route.label(),
            };
            let subtitle = glib::markup_escape_text(&subtitle).to_string();
            if row.title() != title {
                row.set_title(&title);
            }
            if row.subtitle().as_deref() != Some(subtitle.as_str()) {
                row.set_subtitle(&subtitle);
            }
        }
        let selected = self
            .selected
            .as_ref()
            .and_then(|sel| self.snaps.iter().position(|s| &s.display.interface == sel));
        if w.sidebar.selected_row().map(|r| r.index() as usize) != selected {
            match selected.and_then(|i| w.sidebar.row_at_index(i as i32)) {
                Some(r) => w.sidebar.select_row(Some(&r)),
                None => w.sidebar.unselect_all(),
            }
        }
        if self.show_content {
            w.split.set_show_content(true);
        }

        let Some(snap) = self.current() else { return };
        w.content.set_title(snap.name());
        w.page.show(&page::State {
            snap,
            rates: self.rates,
            writable: &self.writable,
            legacy_reason: self.legacy_reason,
            switching: self.switching,
        });
    }
}

fn about(parent: &adw::ApplicationWindow) {
    let about = adw::AboutDialog::builder()
        .application_name(mct::APP_NAME)
        .application_icon(mct::APP_ID)
        .version(mct::VERSION)
        .developer_name("Filip Piękoś")
        .copyright("Copyright (C) 2026 Filip Piękoś")
        .license_type(gtk::License::Gpl20Only)
        .website(mct::WEBSITE)
        .comments("Settings and status for MCT USB displays")
        .build();
    about.add_acknowledgement_section(
        Some("Protocol work builds on"),
        &["cyrozap's notes", "the trigger6 driver"],
    );
    about.present(Some(parent));
}

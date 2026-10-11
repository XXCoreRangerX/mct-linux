// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! mctctl: show and change settings of MCT USB displays.

use std::io::Write;
use std::process::ExitCode;
use std::thread::sleep;
use std::time::{Duration, Instant};

use clap::{Parser, Subcommand, ValueEnum};
use mct::device::{KEYS, SETTABLE};
use mct::ident::{self, Section};
use mct::{discover, switch, usb, Device, Display, Error, Info, RateMeter, Route};
use serde::Serialize;

const DEV_HELP: &str = "DEV is an interface (2-2:1.0), a connector (HDMI-A-3 or card2-HDMI-A-3) \
or an index from \"mctctl list\". It can be left out when only one device is connected.";

#[derive(Parser)]
#[command(
    name = "mctctl",
    version,
    about = "Show and change settings of MCT USB displays (mct driver)",
    after_help = "Exit status: 0 ok, 1 error, 2 invalid input, 3 permission denied.\n\
Set MCT_SYSFS_ROOT to use a sysfs tree other than the one under /."
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Clone, Copy, ValueEnum)]
enum DriverChoice {
    /// The mct kernel driver (recommended)
    Kernel,
    /// wacom-t2d over EVDI, DTU-1031X only
    Legacy,
}

#[derive(Subcommand)]
enum Cmd {
    /// List MCT displays on USB and their driver
    List {
        /// Also show every identifier of each display (as in "info -v");
        /// twice (-vv) adds the technical details
        #[arg(long, short, action = clap::ArgAction::Count)]
        verbose: u8,
        #[arg(long)]
        json: bool,
    },
    /// Show the settings and state of a display
    #[command(after_help = DEV_HELP)]
    Info {
        dev: Option<String>,
        /// Show product, chip and firmware, USB, driver, monitor; twice (-vv)
        /// adds the adapter's internals, USB descriptors and endpoints, the
        /// decoded and raw EDID, driver details
        #[arg(long, short, action = clap::ArgAction::Count)]
        verbose: u8,
        #[arg(long)]
        json: bool,
    },
    /// Print one value
    #[command(after_help = format!("KEY is one of: {}.\n\n{DEV_HELP}", KEYS.join(", ")))]
    Get {
        #[arg(value_name = "[DEV] KEY", num_args = 1..=2, required = true)]
        args: Vec<String>,
    },
    /// Change a setting
    #[command(after_help = format!(
        "KEY is one of: {}.\n  format      t6: auto, rgb24, rgb32, yuv444, nv12\n  all_modes   t6: 0 or 1 (probes the monitor again)\n  lossy       t2: 0 to 8 (0 = always lossless)\n  brightness  t2: 0 (off) to max_brightness\n\n{DEV_HELP}",
        SETTABLE.join(", ")
    ))]
    Set {
        #[arg(value_name = "[DEV] KEY VALUE", num_args = 2..=3, required = true)]
        args: Vec<String>,
    },
    /// Show update rate, data rate and errors
    #[command(after_help = DEV_HELP)]
    Stats {
        dev: Option<String>,
        /// Keep printing, once per interval
        #[arg(long, short)]
        watch: bool,
        /// Seconds between samples
        #[arg(long, default_value_t = 1.0)]
        interval: f64,
        /// Stop after this many lines (with --watch)
        #[arg(long)]
        count: Option<u64>,
        #[arg(long)]
        json: bool,
    },
    /// Switch between the kernel driver and the legacy daemon (asks for authentication)
    #[command(after_help = format!("{}\n\n{DEV_HELP}", switch::LEGACY_NOTE))]
    Driver {
        #[arg(value_name = "[DEV] kernel|legacy", num_args = 1..=2, required = true)]
        args: Vec<String>,
    },
}

/// Writes a line to stdout; a closed pipe (mctctl stats -w | head) ends quietly.
macro_rules! out {
    ($($t:tt)*) => {
        if writeln!(std::io::stdout().lock(), $($t)*).is_err() {
            std::process::exit(0);
        }
    };
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    match run(cli.cmd) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("mctctl: {e}");
            ExitCode::from(e.exit_code() as u8)
        }
    }
}

/// The mct directory of a display, or why there is none.
fn bound(d: &Display) -> Result<&Device, Error> {
    d.device.as_ref().ok_or_else(|| Error::NotKernel {
        interface: d.interface.clone(),
        route: d.route.label(),
    })
}

fn check_key(key: &str) -> Result<(), Error> {
    if KEYS.contains(&key) {
        Ok(())
    } else {
        Err(Error::UnknownKey(key.into()))
    }
}

fn run(cmd: Cmd) -> Result<(), Error> {
    let displays = discover();
    match cmd {
        Cmd::List { verbose, json } => list(&displays, verbose, json),
        Cmd::Info { dev, verbose, json } => {
            info(usb::select(&displays, dev.as_deref())?, verbose, json)
        }
        Cmd::Get { args } => {
            let (dev, key) = match args.as_slice() {
                [key] => (None, key),
                [dev, key] => (Some(dev.as_str()), key),
                _ => unreachable!(),
            };
            // the key first, so a typo gets the clearer error
            check_key(key)?;
            out!("{}", bound(usb::select(&displays, dev)?)?.get(key)?);
            Ok(())
        }
        Cmd::Set { args } => {
            let (dev, key, value) = match args.as_slice() {
                [key, value] => (None, key, value),
                [dev, key, value] => (Some(dev.as_str()), key, value),
                _ => unreachable!(),
            };
            check_key(key)?;
            bound(usb::select(&displays, dev)?)?.set(key, value)
        }
        Cmd::Stats {
            dev,
            watch,
            interval,
            count,
            json,
        } => {
            if !(interval > 0.0 && interval.is_finite()) {
                return Err(Error::Invalid("--interval must be more than 0".into()));
            }
            let dev = bound(usb::select(&displays, dev.as_deref())?)?;
            stats(dev, Duration::from_secs_f64(interval), watch, count, json)
        }
        Cmd::Driver { args } => {
            let (dev, choice) = match args.as_slice() {
                [c] => (None, c),
                [dev, c] => (Some(dev.as_str()), c),
                _ => unreachable!(),
            };
            let choice = DriverChoice::from_str(choice, true).map_err(|_| {
                Error::Invalid(format!(
                    "invalid driver \"{choice}\" (use kernel or legacy)"
                ))
            })?;
            driver(usb::select(&displays, dev)?, choice)
        }
    }
}

fn print_json<T: Serialize>(v: &T) {
    out!("{}", serde_json::to_string_pretty(v).expect("serializable"));
}

/// A display as JSON: the driver's values when bound, the driver and the
/// warnings as text.
#[derive(Serialize)]
struct DisplayJson<'a> {
    #[serde(skip_serializing_if = "Option::is_none")]
    index: Option<usize>,
    // these four come from `info` when the display is bound
    #[serde(skip_serializing_if = "Option::is_none")]
    interface: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    chip: Option<mct::Chip>,
    #[serde(skip_serializing_if = "Option::is_none")]
    model: Option<&'a str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    usb_speed: Option<&'a str>,
    /// mct, legacy, not-loaded, not-bound or another driver's name
    driver: String,
    #[serde(flatten)]
    info: Option<&'a Info>,
    warnings: Vec<String>,
    /// With --verbose: product, chip, USB, driver, monitor.
    #[serde(skip_serializing_if = "Option::is_none")]
    identity: Option<Vec<Section>>,
}

fn notes(d: &Display, info: Option<&Info>) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if let Some(h) = d.route.hint() {
        out.push(format!("{}. {h}", d.route.label()));
    }
    match info {
        Some(i) => out.extend(i.warnings().iter().map(|w| w.text())),
        // without the driver, the USB link is all we know
        None if d.chip == mct::Chip::T6 && d.usb_speed.as_deref() == Some("usb2") => {
            out.push(mct::Warning::Usb2.text())
        }
        None => {}
    }
    out
}

fn display_json<'a>(
    index: Option<usize>,
    d: &'a Display,
    info: Option<&'a Info>,
    verbose: u8,
) -> DisplayJson<'a> {
    let unbound = info.is_none();
    DisplayJson {
        index,
        interface: unbound.then_some(d.interface.as_str()),
        chip: unbound.then_some(d.chip),
        model: d.product.as_deref().filter(|_| unbound),
        usb_speed: d.usb_speed.as_deref().filter(|_| unbound),
        driver: d.route.short(),
        info,
        warnings: notes(d, info),
        identity: (verbose > 0).then(|| sections(d, info, verbose)),
    }
}

/// The product's short name, else the USB product string.
fn name<'a>(d: &'a Display, info: Option<&'a Info>) -> &'a str {
    ident::product(d, info)
        .map(|p| p.short)
        .or_else(|| info.map(|i| i.name()))
        .unwrap_or(d.name())
}

/// Settings and counters, for "info -v" after the identity sections.
const STATE_KEYS: &[(&str, &str)] = &[
    ("mode", "Mode"),
    ("pixel_format", "Pixel format"),
    ("format", "Format setting"),
    ("all_modes", "All modes"),
    ("lossy", "Lossy"),
    ("brightness", "Brightness"),
    ("max_brightness", "Max brightness"),
    ("updates", "Updates"),
    ("bytes", "Bytes sent"),
    ("errors", "Transfer errors"),
    ("recoveries", "Device restarts"),
];

/// The identity sections for a verbosity level: details only from -vv.
fn sections(d: &Display, info: Option<&Info>, verbose: u8) -> Vec<Section> {
    ident::identify(d, info)
        .into_iter()
        .filter(|s| verbose >= 2 || !s.detail)
        .collect()
}

fn verbose_text(d: &Display, info: Option<&Info>, verbose: u8) -> String {
    let mut sections = sections(d, info, verbose);
    if let Some(i) = info {
        let fields: Vec<ident::Field> = STATE_KEYS
            .iter()
            .filter_map(|(key, label)| {
                i.value(key).map(|value| ident::Field {
                    key: key.to_string(),
                    label: label.to_string(),
                    value,
                })
            })
            .collect();
        if !fields.is_empty() {
            sections.push(Section {
                key: "state",
                title: "State",
                detail: false,
                fields,
            });
        }
    }
    ident::sections_text(&sections, "")
}

fn read_info(d: &Display) -> Result<Option<Info>, Error> {
    match &d.device {
        Some(dev) => dev.info().map(Some),
        None => Ok(None),
    }
}

fn info(d: &Display, verbose: u8, json: bool) -> Result<(), Error> {
    let info = read_info(d)?;
    if json {
        print_json(&display_json(None, d, info.as_ref(), verbose));
        return Ok(());
    }
    if verbose > 0 {
        out!("{} ({})", name(d, info.as_ref()), d.interface);
        out!("{}", verbose_text(d, info.as_ref(), verbose).trim_end());
        for n in notes(d, info.as_ref()) {
            out!("warning: {n}");
        }
        return Ok(());
    }
    let mut entries: Vec<(&str, String)> = match &info {
        Some(i) => i.entries(),
        None => {
            let mut v = vec![
                ("interface", d.interface.clone()),
                ("chip", d.chip.to_string()),
            ];
            if let Some(p) = &d.product {
                v.push(("model", p.clone()));
            }
            if let Some(s) = &d.usb_speed {
                v.push(("usb_speed", s.clone()));
            }
            v
        }
    };
    entries.insert(2.min(entries.len()), ("driver", d.route.label()));
    let width = KEYS.iter().map(|k| k.len()).max().unwrap_or(0) + 1;
    for (k, v) in entries {
        out!("{:width$}  {v}", format!("{k}:"));
    }
    for n in notes(d, info.as_ref()) {
        out!("warning: {n}");
    }
    Ok(())
}

fn list(displays: &[Display], verbose: u8, json: bool) -> Result<(), Error> {
    let mut rows = Vec::new();
    for d in displays {
        match read_info(d) {
            Ok(i) => rows.push((d, i)),
            // unplugged while we looked
            Err(Error::Gone(_)) => {}
            Err(e) => return Err(e),
        }
    }
    if json {
        let v: Vec<DisplayJson> = rows
            .iter()
            .enumerate()
            .map(|(i, (d, info))| display_json(Some(i), d, info.as_ref(), verbose))
            .collect();
        print_json(&v);
        return Ok(());
    }
    if rows.is_empty() {
        eprintln!("no MCT displays found");
        return Ok(());
    }
    // a "!" after the index marks displays with notes, listed below the table
    let header = [
        "#",
        "INTERFACE",
        "CHIP",
        "MODEL",
        "CONNECTOR",
        "MODE",
        "FORMAT",
        "DRIVER",
    ];
    let mut table = vec![header.map(String::from).to_vec()];
    for (i, (d, info)) in rows.iter().enumerate() {
        let mark = if notes(d, info.as_ref()).is_empty() {
            ""
        } else {
            "!"
        };
        let dash = || "-".to_string();
        let format = match info {
            Some(i) if i.chip == mct::Chip::T2 => {
                i.lossy.map(|l| format!("lossy {l}")).unwrap_or_else(dash)
            }
            Some(i) => i.format_summary(),
            None => dash(),
        };
        table.push(vec![
            format!("{i}{mark}"),
            d.interface.clone(),
            d.chip.to_string(),
            name(d, info.as_ref()).to_string(),
            info.as_ref()
                .and_then(|i| i.connector_short())
                .unwrap_or("-")
                .to_string(),
            info.as_ref()
                .and_then(|i| i.mode.clone())
                .unwrap_or_else(dash),
            format,
            d.route.short(),
        ]);
    }
    let widths: Vec<usize> = (0..header.len())
        .map(|c| {
            table
                .iter()
                .map(|r| r[c].chars().count())
                .max()
                .unwrap_or(0)
        })
        .collect();
    for r in &table {
        let mut line = String::new();
        for (c, cell) in r.iter().enumerate() {
            line.push_str(cell);
            if c + 1 < r.len() {
                line.push_str(&" ".repeat(widths[c] - cell.chars().count() + 2));
            }
        }
        out!("{line}");
    }
    for (i, (d, info)) in rows.iter().enumerate() {
        let place = info
            .as_ref()
            .and_then(|i| i.connector_short())
            .unwrap_or(&d.interface);
        for n in notes(d, info.as_ref()) {
            out!("{i}! {place}: {n}");
        }
    }
    if verbose > 0 {
        for (i, (d, info)) in rows.iter().enumerate() {
            out!("");
            out!("#{i} {} ({})", name(d, info.as_ref()), d.interface);
            let text = verbose_text(d, info.as_ref(), verbose);
            for line in text.lines() {
                out!("  {line}");
            }
        }
    }
    Ok(())
}

fn driver(d: &Display, choice: DriverChoice) -> Result<(), Error> {
    let target = match choice {
        DriverChoice::Kernel => Route::Kernel,
        DriverChoice::Legacy => Route::Legacy,
    };
    if d.route == target {
        out!("{}: already using {}", d.interface, d.route.label());
        return Ok(());
    }
    match choice {
        DriverChoice::Kernel => switch::to_kernel(d)?,
        DriverChoice::Legacy => switch::to_legacy(d)?,
    }
    // the new driver takes a moment to bind or the daemon to start
    let start = Instant::now();
    let mut now = d.route.clone();
    while start.elapsed() < Duration::from_secs(5) {
        sleep(Duration::from_millis(250));
        if let Some(x) = discover().into_iter().find(|x| x.interface == d.interface) {
            now = x.route;
            if now == target {
                break;
            }
        }
    }
    out!("{}: {}", d.interface, now.label());
    Ok(())
}

#[derive(Serialize)]
struct StatsLine {
    interface: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    connector: Option<String>,
    updates: u64,
    bytes: u64,
    errors: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    recoveries: Option<u64>,
    updates_per_s: f64,
    mb_per_s: f64,
}

fn stats(
    dev: &Device,
    interval: Duration,
    watch: bool,
    count: Option<u64>,
    json: bool,
) -> Result<(), Error> {
    let mut meter = RateMeter::new();
    let mut take = || -> Result<(Info, Option<mct::Rates>), Error> {
        let info = dev.info()?;
        let r = meter.sample(
            Instant::now(),
            info.updates.unwrap_or(0),
            info.bytes.unwrap_or(0),
        );
        Ok((info, r))
    };
    take()?;
    let mut printed = 0u64;
    loop {
        sleep(interval);
        let (info, rates) = take()?;
        let rates = rates.unwrap_or_default();
        let line = StatsLine {
            interface: info.interface.clone(),
            connector: info.connector_short().map(String::from),
            updates: info.updates.unwrap_or(0),
            bytes: info.bytes.unwrap_or(0),
            errors: info.errors.unwrap_or(0),
            recoveries: info.recoveries,
            updates_per_s: rates.updates_per_s,
            mb_per_s: rates.mb_per_s,
        };
        if json {
            // one object per line, so --watch output can be streamed
            out!("{}", serde_json::to_string(&line).expect("serializable"));
        } else {
            let mut s = format!(
                "{}: {:.1} updates/s, {:.2} MB/s, errors {}",
                line.connector.as_deref().unwrap_or(&line.interface),
                line.updates_per_s,
                line.mb_per_s,
                line.errors
            );
            if let Some(r) = line.recoveries {
                s.push_str(&format!(", recoveries {r}"));
            }
            s.push_str(&format!(
                " ({} updates, {} sent)",
                line.updates,
                mct::text::bytes(line.bytes)
            ));
            out!("{s}");
        }
        printed += 1;
        if !watch || count.is_some_and(|c| printed >= c) {
            return Ok(());
        }
    }
}

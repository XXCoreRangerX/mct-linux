// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Switching the tablet between the kernel driver and the legacy daemon.
//!
//! Binding and unbinding need root: they go through `mct-manager-helper`
//! run by pkexec. Starting and stopping wacom-t2d is done as the user.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread::sleep;
use std::time::{Duration, Instant};

use crate::device::{sysfs_root, Error};
use crate::usb::{module_loaded, process_running, Display, Route, LEGACY_DAEMON};

pub const HELPER: &str = "mct-manager-helper";
const UNIT: &str = "wacom-t2d.service";

pub const LEGACY_NOTE: &str = "The kernel driver is recommended. The legacy daemon \
     (wacom-t2d over EVDI) is for kernels older than 7.2 or debugging. On NixOS the \
     permanent choice is hardware.mct.driver = \"kernel\" or \"evdi\".";

fn in_path(prog: &str) -> bool {
    std::env::var_os("PATH")
        .map(|p| std::env::split_paths(&p).any(|d| d.join(prog).is_file()))
        .unwrap_or(false)
}

fn quiet(cmd: &mut Command) -> bool {
    cmd.stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

fn user_unit_exists() -> bool {
    quiet(Command::new("systemctl").args(["--user", "cat", UNIT]))
}

/// Why the legacy daemon can't be used here, if it can't.
pub fn legacy_unavailable(display: &Display) -> Option<&'static str> {
    legacy_unavailable_in(&sysfs_root(), display)
}

pub fn legacy_unavailable_in(root: &Path, display: &Display) -> Option<&'static str> {
    if !display.is_tablet() {
        return Some("the legacy daemon is for the DTU-1031X only");
    }
    if root != Path::new("/") {
        // a fake tree: only look at the tree
        return (!module_loaded(root, "evdi")).then_some("the evdi module is not installed");
    }
    if !in_path(LEGACY_DAEMON) && !user_unit_exists() {
        return Some("wacom-t2d is not installed");
    }
    if !module_loaded(root, "evdi") && !quiet(Command::new("modinfo").args(["-n", "evdi"])) {
        return Some("the evdi module is not installed");
    }
    None
}

/// The helper next to our binary (`<prefix>/libexec`), or from PATH.
pub fn helper_path() -> Option<PathBuf> {
    if let Some(prefix) = std::env::current_exe()
        .ok()
        .and_then(|e| Some(e.parent()?.parent()?.to_path_buf()))
    {
        let p = prefix.join("libexec").join(HELPER);
        if p.is_file() {
            return Some(p);
        }
    }
    std::env::var_os("PATH").and_then(|p| {
        std::env::split_paths(&p)
            .map(|d| d.join(HELPER))
            .find(|p| p.is_file())
    })
}

/// Runs `pkexec mct-manager-helper VERB INTERFACE`.
fn helper(verb: &str, interface: &str) -> Result<(), Error> {
    let helper =
        helper_path().ok_or_else(|| Error::Switch(format!("{HELPER} is not installed")))?;
    let out = Command::new("pkexec")
        .arg(&helper)
        .args([verb, interface])
        .stdin(Stdio::null())
        .output()
        .map_err(|e| Error::Switch(format!("cannot run pkexec: {e}")))?;
    if out.status.success() {
        return Ok(());
    }
    let msg = String::from_utf8_lossy(&out.stderr).trim().to_string();
    Err(Error::Switch(match out.status.code() {
        Some(126) | Some(127) if msg.is_empty() || msg.contains("Not authorized") => {
            "not authorized".into()
        }
        _ if msg.is_empty() => format!("{HELPER} {verb} failed"),
        _ => msg,
    }))
}

fn wait_until(timeout: Duration, mut done: impl FnMut() -> bool) -> bool {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if done() {
            return true;
        }
        sleep(Duration::from_millis(100));
    }
    done()
}

/// Switching acts on the real system: refuse when looking at a fake tree.
fn real_root() -> Result<(), Error> {
    if sysfs_root() == Path::new("/") {
        Ok(())
    } else {
        Err(Error::Switch(
            "switching needs the real /sys (MCT_SYSFS_ROOT is set)".into(),
        ))
    }
}

/// Our wacom-t2d processes, by pid.
fn daemon_pids() -> Vec<String> {
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return Vec::new();
    };
    entries
        .flatten()
        .filter(|e| {
            std::fs::read_to_string(e.path().join("comm")).is_ok_and(|c| c.trim() == LEGACY_DAEMON)
        })
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect()
}

/// Unbinds the tablet from mct and starts wacom-t2d.
pub fn to_legacy(display: &Display) -> Result<(), Error> {
    if let Some(reason) = legacy_unavailable(display) {
        return Err(Error::Switch(reason.into()));
    }
    if display.route == Route::Legacy {
        return Ok(());
    }
    real_root()?;
    helper("unbind", &display.interface)?;
    let started = if user_unit_exists() {
        quiet(Command::new("systemctl").args(["--user", "start", UNIT]))
    } else {
        Command::new(LEGACY_DAEMON)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .is_ok()
    };
    if !started {
        return Err(Error::Switch(
            "unbound from mct, but wacom-t2d did not start".into(),
        ));
    }
    Ok(())
}

/// Stops wacom-t2d (if running) and binds the display to mct.
pub fn to_kernel(display: &Display) -> Result<(), Error> {
    if display.route == Route::Kernel {
        return Ok(());
    }
    real_root()?;
    if display.is_tablet() && process_running(Path::new("/"), LEGACY_DAEMON) {
        if user_unit_exists() {
            quiet(Command::new("systemctl").args(["--user", "stop", UNIT]));
        }
        for pid in daemon_pids() {
            quiet(Command::new("kill").arg(&pid));
        }
        if !wait_until(Duration::from_secs(3), || daemon_pids().is_empty()) {
            return Err(Error::Switch(
                "wacom-t2d is still running (started by another user?)".into(),
            ));
        }
    }
    helper("bind", &display.interface)
}

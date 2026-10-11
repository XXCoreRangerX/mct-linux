// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Filip Piękoś <mail@filippiekos.com>

//! Update and data rates from the `updates` and `bytes` counters.

use std::time::Instant;

use serde::Serialize;

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize)]
pub struct Rates {
    pub updates_per_s: f64,
    /// Megabytes (10^6 bytes) per second.
    pub mb_per_s: f64,
}

/// Turns successive counter samples into rates.
#[derive(Clone, Debug, Default)]
pub struct RateMeter {
    last: Option<(Instant, u64, u64)>,
}

impl RateMeter {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reset(&mut self) {
        self.last = None;
    }

    /// Adds a sample; returns the rates since the previous one, or `None`
    /// for the first sample and after the counters went backwards (the
    /// device was probed again).
    pub fn sample(&mut self, at: Instant, updates: u64, bytes: u64) -> Option<Rates> {
        let prev = self.last.replace((at, updates, bytes));
        let (t0, u0, b0) = prev?;
        let dt = at.checked_duration_since(t0)?.as_secs_f64();
        if dt <= 0.0 || updates < u0 || bytes < b0 {
            return None;
        }
        Some(Rates {
            updates_per_s: (updates - u0) as f64 / dt,
            mb_per_s: (bytes - b0) as f64 / dt / 1e6,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn rates() {
        let t = Instant::now();
        let mut m = RateMeter::new();
        assert_eq!(m.sample(t, 100, 1_000_000), None);
        let r = m
            .sample(t + Duration::from_millis(500), 130, 6_000_000)
            .unwrap();
        assert!((r.updates_per_s - 60.0).abs() < 1e-9);
        assert!((r.mb_per_s - 10.0).abs() < 1e-9);
        // counters reset: no rate, next sample works again
        assert_eq!(m.sample(t + Duration::from_secs(1), 5, 10), None);
        let r = m.sample(t + Duration::from_secs(2), 5, 10).unwrap();
        assert_eq!(r, Rates::default());
    }
}

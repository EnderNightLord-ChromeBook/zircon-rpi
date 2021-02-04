// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::datatypes::{HttpsSample, Phase};
use crate::diagnostics::Diagnostics;
use httpdate_hyper::HttpsDateErrorType;
use parking_lot::Mutex;

/// A fake `Diagnostics` implementation useful for verifying unittests.
pub struct FakeDiagnostics {
    /// An ordered list of the successes received since the last reset.
    successes: Mutex<Vec<HttpsSample>>,
    /// An ordered list of the failures received since the last reset.
    failures: Mutex<Vec<HttpsDateErrorType>>,
    /// An ordered list of phase updates received since the last reset.
    phases: Mutex<Vec<Phase>>,
}

impl FakeDiagnostics {
    /// Constructs a new `FakeDiagnostics`.
    pub fn new() -> Self {
        FakeDiagnostics {
            successes: Mutex::new(Vec::new()),
            failures: Mutex::new(Vec::new()),
            phases: Mutex::new(Vec::new()),
        }
    }

    /// Returns a copy of the successes received since the last reset.
    pub fn successes(&self) -> Vec<HttpsSample> {
        self.successes.lock().clone()
    }

    /// Returns a copy of the failures received since the last reset.
    pub fn failures(&self) -> Vec<HttpsDateErrorType> {
        self.failures.lock().clone()
    }

    /// Returns a copy of the phase updates received since the last reset.
    pub fn phase_updates(&self) -> Vec<Phase> {
        self.phases.lock().clone()
    }

    /// Clears all recorded interactions.
    pub fn reset(&self) {
        self.successes.lock().clear();
        self.failures.lock().clear();
        self.phases.lock().clear();
    }
}

impl Diagnostics for FakeDiagnostics {
    fn success(&self, sample: &HttpsSample) {
        self.successes.lock().push(sample.clone());
    }

    fn failure(&self, error: &HttpsDateErrorType) {
        self.failures.lock().push(*error);
    }

    fn phase_update(&self, phase: &Phase) {
        self.phases.lock().push(*phase);
    }
}

impl<T: AsRef<FakeDiagnostics> + Send + Sync> Diagnostics for T {
    fn success(&self, sample: &HttpsSample) {
        self.as_ref().success(sample);
    }

    fn failure(&self, error: &HttpsDateErrorType) {
        self.as_ref().failure(error);
    }

    fn phase_update(&self, phase: &Phase) {
        self.as_ref().phase_update(phase);
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::datatypes::Poll;
    use fuchsia_zircon as zx;
    use lazy_static::lazy_static;

    lazy_static! {
        static ref SUCCESS_1: HttpsSample = HttpsSample {
            utc: zx::Time::from_nanos(111_111_111),
            monotonic: zx::Time::from_nanos(222_222_222),
            standard_deviation: zx::Duration::from_millis(235),
            final_bound_size: zx::Duration::from_millis(100),
            polls: vec![
                Poll::with_round_trip_time(zx::Duration::from_millis(25)),
                Poll::with_round_trip_time(zx::Duration::from_millis(50)),
            ],
        };
        static ref SUCCESS_2: HttpsSample = HttpsSample {
            utc: zx::Time::from_nanos(333_333_333),
            monotonic: zx::Time::from_nanos(444_444_444),
            standard_deviation: zx::Duration::from_millis(236),
            final_bound_size: zx::Duration::from_millis(101),
            polls: vec![
                Poll {
                    round_trip_time: zx::Duration::from_millis(26),
                    center_offset: Some(zx::Duration::from_millis(168))
                },
                Poll {
                    round_trip_time: zx::Duration::from_millis(51),
                    center_offset: Some(zx::Duration::from_millis(-250))
                },
            ],
        };
    }
    const ERROR_1: HttpsDateErrorType = HttpsDateErrorType::NetworkError;
    const ERROR_2: HttpsDateErrorType = HttpsDateErrorType::InvalidHostname;
    const PHASE_1: Phase = Phase::Initial;
    const PHASE_2: Phase = Phase::Maintain;

    #[test]
    fn log_and_reset_successes() {
        let diagnostics = FakeDiagnostics::new();
        assert_eq!(diagnostics.successes(), vec![]);

        diagnostics.success(&*SUCCESS_1);
        assert_eq!(diagnostics.successes(), vec![SUCCESS_1.clone()]);

        diagnostics.success(&*SUCCESS_2);
        assert_eq!(diagnostics.successes(), vec![SUCCESS_1.clone(), SUCCESS_2.clone()]);

        diagnostics.reset();
        assert_eq!(diagnostics.successes(), vec![]);
    }

    #[test]
    fn log_and_reset_failures() {
        let diagnostics = FakeDiagnostics::new();
        assert_eq!(diagnostics.failures(), vec![]);

        diagnostics.failure(&ERROR_1);
        assert_eq!(diagnostics.failures(), vec![ERROR_1]);

        diagnostics.failure(&ERROR_2);
        assert_eq!(diagnostics.failures(), vec![ERROR_1, ERROR_2]);

        diagnostics.reset();
        assert_eq!(diagnostics.failures(), vec![]);
    }

    #[test]
    fn log_and_reset_phases() {
        let diagnostics = FakeDiagnostics::new();
        assert_eq!(diagnostics.phase_updates(), vec![]);

        diagnostics.phase_update(&PHASE_1);
        assert_eq!(diagnostics.phase_updates(), vec![PHASE_1]);

        diagnostics.phase_update(&PHASE_2);
        assert_eq!(diagnostics.phase_updates(), vec![PHASE_1, PHASE_2]);

        diagnostics.reset();
        assert_eq!(diagnostics.phase_updates(), vec![]);
    }
}

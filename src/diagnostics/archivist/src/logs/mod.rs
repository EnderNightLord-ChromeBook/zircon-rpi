// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod budget;
pub mod buffer;
pub mod container;
pub mod debuglog;
pub mod error;
pub mod listener;
pub mod message;
pub mod redact;
pub mod socket;
pub mod stats;
#[cfg(test)]
pub mod testing;

pub use debuglog::{convert_debuglog_to_log_message, KernelDebugLog};
pub use message::Message;

#[cfg(test)]
mod tests {
    use crate::{
        container::ComponentIdentity,
        events::types::ComponentIdentifier,
        logs::{message::LegacySeverity, testing::*},
    };
    use diagnostics_data::{DROPPED_LABEL, MESSAGE_LABEL, PID_LABEL, TAG_LABEL, TID_LABEL};
    use diagnostics_stream::{Argument, Record, Severity as StreamSeverity, Value};
    use fidl_fuchsia_logger::{LogFilterOptions, LogLevelFilter, LogMessage};
    use fuchsia_async as fasync;
    use fuchsia_inspect::{assert_inspect_tree, testing::AnyProperty};
    use fuchsia_zircon as zx;
    use std::sync::Arc;

    #[fasync::run_singlethreaded(test)]
    async fn test_log_manager_simple() {
        TestHarness::new().manager_test(false).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_log_manager_dump() {
        TestHarness::new().manager_test(true).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn unfiltered_stats() {
        let first_packet = setup_default_packet();
        let first_message = LogMessage {
            pid: first_packet.metadata.pid,
            tid: first_packet.metadata.tid,
            time: first_packet.metadata.time,
            dropped_logs: first_packet.metadata.dropped_logs,
            severity: first_packet.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };

        let (mut second_packet, mut second_message) = (first_packet.clone(), first_message.clone());
        second_packet.metadata.pid = 0;
        second_message.pid = second_packet.metadata.pid;

        let (mut third_packet, mut third_message) = (second_packet.clone(), second_message.clone());
        third_packet.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        third_message.severity = third_packet.metadata.severity;

        let (fourth_packet, fourth_message) = (third_packet.clone(), third_message.clone());

        let (mut fifth_packet, mut fifth_message) = (fourth_packet.clone(), fourth_message.clone());
        fifth_packet.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        fifth_message.severity = fifth_packet.metadata.severity;

        let mut harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(vec![
            first_packet,
            second_packet,
            third_packet,
            fourth_packet,
            fifth_packet,
        ]);
        drop(stream);

        let log_stats_tree = harness
            .filter_test(
                vec![first_message, second_message, third_message, fourth_message, fifth_message],
                None,
            )
            .await;

        assert_inspect_tree!(
            log_stats_tree,
            root: contains {
                sources: {
                    "UNKNOWN": {
                        url: "fuchsia-pkg://UNKNOWN",
                        logs: {
                            last_timestamp: AnyProperty,
                            total: 5u64,
                            dropped: 0u64,
                            trace: 0u64,
                            debug: 0u64,
                            info: 2u64,
                            warn: 2u64,
                            error: 1u64,
                            fatal: 0u64,
                        },
                    },
                }
            }
        );
    }

    macro_rules! attributed_inspect_two_streams_different_identities_by_reader {
        (
            $harness:ident,
            $log_reader1:ident @ $foo_moniker:literal,
            $log_reader2:ident @ $bar_moniker:literal,
        ) => {{
            let mut packet = setup_default_packet();
            let message = LogMessage {
                pid: packet.metadata.pid,
                tid: packet.metadata.tid,
                time: packet.metadata.time,
                dropped_logs: packet.metadata.dropped_logs,
                severity: packet.metadata.severity,
                msg: String::from("BBBBB"),
                tags: vec![String::from("AAAAA")],
            };

            let mut packet2 = packet.clone();
            packet2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
            let mut message2 = message.clone();
            message2.severity = packet2.metadata.severity;

            let mut foo_stream = $harness.create_stream_from_log_reader($log_reader1);
            foo_stream.write_packet(&mut packet);

            let mut bar_stream = $harness.create_stream_from_log_reader($log_reader2);
            bar_stream.write_packet(&mut packet2);
            drop((foo_stream, bar_stream));

            let log_stats_tree = $harness.filter_test(vec![message, message2], None).await;

            assert_inspect_tree!(
                log_stats_tree,
                root: contains {
                    sources: {
                        $foo_moniker: {
                            url: "http://foo.com",
                            logs: {
                                last_timestamp: AnyProperty,
                                total: 1u64,
                                dropped: 0u64,
                                trace: 0u64,
                                debug: 0u64,
                                info: 0u64,
                                warn: 1u64,
                                error: 0u64,
                                fatal: 0u64,
                            },
                        },
                        $bar_moniker: {
                            url: "http://bar.com",
                            logs: {
                                last_timestamp: AnyProperty,
                                total: 1u64,
                                dropped: 0u64,
                                trace: 0u64,
                                debug: 0u64,
                                info: 0u64,
                                warn: 0u64,
                                error: 1u64,
                                fatal: 0u64,
                            },
                        },
                    },
                }
            );
        }}
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_streams_different_identities() {
        let mut harness = TestHarness::with_retained_sinks();

        let log_reader1 =
            harness.create_default_reader(ComponentIdentity::from_identifier_and_url(
                &ComponentIdentifier::Legacy {
                    moniker: vec![".", "foo"].into(),
                    instance_id: "0".into(),
                },
                "http://foo.com",
            ));

        let log_reader2 =
            harness.create_default_reader(ComponentIdentity::from_identifier_and_url(
                &ComponentIdentifier::Legacy {
                    moniker: vec![".", "bar"].into(),
                    instance_id: "0".into(),
                },
                "http://bar.com",
            ));

        attributed_inspect_two_streams_different_identities_by_reader!(
            harness,
            log_reader1 @ "./foo",
            log_reader2 @ "./bar",
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_v2_streams_different_identities() {
        let mut harness = TestHarness::with_retained_sinks();
        let log_reader1 = harness.create_event_stream_reader("./foo:0", "http://foo.com");
        let log_reader2 = harness.create_event_stream_reader("./bar:0", "http://bar.com");

        attributed_inspect_two_streams_different_identities_by_reader!(
            harness,
            log_reader1 @ "foo",
            log_reader2 @ "bar",
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn attributed_inspect_two_mixed_streams_different_identities() {
        let mut harness = TestHarness::with_retained_sinks();
        let log_reader1 = harness.create_event_stream_reader("./foo:0", "http://foo.com");
        let log_reader2 =
            harness.create_default_reader(ComponentIdentity::from_identifier_and_url(
                &ComponentIdentifier::Legacy {
                    moniker: vec![".", "bar"].into(),
                    instance_id: "0".into(),
                },
                "http://bar.com",
            ));

        attributed_inspect_two_streams_different_identities_by_reader!(
            harness,
            log_reader1 @ "foo",
            log_reader2 @ "./bar",
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_pid() {
        let p = setup_default_packet();
        let mut p2 = p.clone();
        p2.metadata.pid = 0;
        let lm = LogMessage {
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            time: p.metadata.time,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: true,
            pid: 1,
            filter_by_tid: false,
            tid: 0,
            min_severity: LogLevelFilter::None,
            verbosity: 0,
            tags: vec![],
        };

        let mut harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(vec![p, p2]);
        drop(stream);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_tid() {
        let mut p = setup_default_packet();
        p.metadata.pid = 0;
        let mut p2 = p.clone();
        p2.metadata.tid = 0;
        let lm = LogMessage {
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            time: p.metadata.time,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: false,
            pid: 1,
            filter_by_tid: true,
            tid: 1,
            min_severity: LogLevelFilter::None,
            verbosity: 0,
            tags: vec![],
        };

        let mut harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(vec![p, p2]);
        drop(stream);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_min_severity() {
        let p = setup_default_packet();
        let mut p2 = p.clone();
        p2.metadata.pid = 0;
        p2.metadata.tid = 0;
        p2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        let mut p3 = p.clone();
        p3.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        let mut p4 = p.clone();
        p4.metadata.severity = 0x70; // custom
        let mut p5 = p.clone();
        p5.metadata.severity = LogLevelFilter::Fatal.into_primitive().into();
        let lm = LogMessage {
            pid: p2.metadata.pid,
            tid: p2.metadata.tid,
            time: p2.metadata.time,
            dropped_logs: p2.metadata.dropped_logs,
            severity: p2.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: false,
            pid: 1,
            filter_by_tid: false,
            tid: 1,
            min_severity: LogLevelFilter::Error,
            verbosity: 0,
            tags: vec![],
        };

        let mut harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(vec![p, p2, p3, p4, p5]);
        drop(stream);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_combination() {
        let mut p = setup_default_packet();
        p.metadata.pid = 0;
        p.metadata.tid = 0;
        let mut p2 = p.clone();
        p2.metadata.severity = LogLevelFilter::Error.into_primitive().into();
        let mut p3 = p.clone();
        p3.metadata.pid = 1;
        let lm = LogMessage {
            pid: p2.metadata.pid,
            tid: p2.metadata.tid,
            time: p2.metadata.time,
            dropped_logs: p2.metadata.dropped_logs,
            severity: p2.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let options = LogFilterOptions {
            filter_by_pid: true,
            pid: 0,
            filter_by_tid: false,
            tid: 1,
            min_severity: LogLevelFilter::Error,
            verbosity: 0,
            tags: vec![],
        };

        let mut harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(vec![p, p2, p3]);
        drop(stream);
        harness.filter_test(vec![lm], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_filter_by_tags() {
        let mut p = setup_default_packet();
        let mut p2 = p.clone();
        // p tags - "DDDDD"
        p.fill_data(1..6, 68);

        p2.metadata.pid = 0;
        p2.metadata.tid = 0;
        p2.data[6] = 5;
        // p2 tag - "AAAAA", "BBBBB"
        // p2 msg - "CCCCC"
        p2.fill_data(13..18, 67);

        let lm1 = LogMessage {
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            time: p.metadata.time,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("DDDDD")],
        };
        let lm2 = LogMessage {
            pid: p2.metadata.pid,
            tid: p2.metadata.tid,
            time: p2.metadata.time,
            dropped_logs: p2.metadata.dropped_logs,
            severity: p2.metadata.severity,
            msg: String::from("CCCCC"),
            tags: vec![String::from("AAAAA"), String::from("BBBBB")],
        };
        let options = LogFilterOptions {
            filter_by_pid: false,
            pid: 1,
            filter_by_tid: false,
            tid: 1,
            min_severity: LogLevelFilter::None,
            verbosity: 0,
            tags: vec![String::from("BBBBB"), String::from("DDDDD")],
        };

        let mut harness = TestHarness::new();
        let mut stream = harness.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(vec![p, p2]);
        drop(stream);
        harness.filter_test(vec![lm1, lm2], Some(options)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_structured_log() {
        let logs = vec![
            Record {
                timestamp: 6,
                severity: StreamSeverity::Info,
                arguments: vec![Argument {
                    name: MESSAGE_LABEL.into(),
                    value: Value::Text("hi".to_string()),
                }],
            },
            Record { timestamp: 14, severity: StreamSeverity::Error, arguments: vec![] },
            Record {
                timestamp: 19,
                severity: StreamSeverity::Warn,
                arguments: vec![
                    Argument { name: PID_LABEL.into(), value: Value::UnsignedInt(0x1d1) },
                    Argument { name: TID_LABEL.into(), value: Value::UnsignedInt(0x1d2) },
                    Argument { name: DROPPED_LABEL.into(), value: Value::UnsignedInt(23) },
                    Argument { name: TAG_LABEL.into(), value: Value::Text(String::from("tag")) },
                    Argument {
                        name: MESSAGE_LABEL.into(),
                        value: Value::Text(String::from("message")),
                    },
                ],
            },
            Record {
                timestamp: 21,
                severity: StreamSeverity::Warn,
                arguments: vec![
                    Argument { name: TAG_LABEL.into(), value: Value::Text(String::from("tag-1")) },
                    Argument { name: TAG_LABEL.into(), value: Value::Text(String::from("tag-2")) },
                ],
            },
        ];

        let expected_logs = vec![
            LogMessage {
                pid: zx::sys::ZX_KOID_INVALID,
                tid: zx::sys::ZX_KOID_INVALID,
                time: 6,
                severity: LegacySeverity::Info.for_listener(),
                dropped_logs: 0,
                msg: String::from("hi"),
                tags: vec!["UNKNOWN:0".to_owned()],
            },
            LogMessage {
                pid: zx::sys::ZX_KOID_INVALID,
                tid: zx::sys::ZX_KOID_INVALID,
                time: 14,
                severity: LegacySeverity::Error.for_listener(),
                dropped_logs: 0,
                msg: String::from(""),
                tags: vec!["UNKNOWN:0".to_owned()],
            },
            LogMessage {
                pid: 0x1d1,
                tid: 0x1d2,
                time: 19,
                severity: LegacySeverity::Warn.for_listener(),
                dropped_logs: 23,
                msg: String::from("message"),
                tags: vec![String::from("tag")],
            },
            LogMessage {
                pid: zx::sys::ZX_KOID_INVALID,
                tid: zx::sys::ZX_KOID_INVALID,
                time: 21,
                severity: LegacySeverity::Warn.for_listener(),
                dropped_logs: 0,
                msg: String::from(""),
                tags: vec![String::from("tag-1"), String::from("tag-2")],
            },
        ];
        let mut harness = TestHarness::new();
        let mut stream = harness.create_structured_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packets(logs);
        drop(stream);
        harness.filter_test(expected_logs, None).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_debuglog_drainer() {
        let log1 = TestDebugEntry::new("log1".as_bytes());
        let log2 = TestDebugEntry::new("log2".as_bytes());
        let log3 = TestDebugEntry::new("log3".as_bytes());

        let klog_reader = TestDebugLog::new();
        klog_reader.enqueue_read_entry(&log1);
        klog_reader.enqueue_read_entry(&log2);
        // logs recieved after kernel indicates no logs should be read
        klog_reader.enqueue_read_fail(zx::Status::SHOULD_WAIT);
        klog_reader.enqueue_read_entry(&log3);
        klog_reader.enqueue_read_fail(zx::Status::SHOULD_WAIT);

        let expected_logs = vec![
            LogMessage {
                pid: log1.pid,
                tid: log1.tid,
                time: log1.timestamp,
                dropped_logs: 0,
                severity: fidl_fuchsia_logger::LogLevelFilter::Info as i32,
                msg: String::from("log1"),
                tags: vec![String::from("klog")],
            },
            LogMessage {
                pid: log2.pid,
                tid: log2.tid,
                time: log2.timestamp,
                dropped_logs: 0,
                severity: fidl_fuchsia_logger::LogLevelFilter::Info as i32,
                msg: String::from("log2"),
                tags: vec![String::from("klog")],
            },
            LogMessage {
                pid: log3.pid,
                tid: log3.tid,
                time: log3.timestamp,
                dropped_logs: 0,
                severity: fidl_fuchsia_logger::LogLevelFilter::Info as i32,
                msg: String::from("log3"),
                tags: vec![String::from("klog")],
            },
        ];

        let klog_stats_tree = debuglog_test(expected_logs, klog_reader).await;
        assert_inspect_tree!(
            klog_stats_tree,
            root: contains {
                "sources": {
                    "klog": {
                        url: "fuchsia-boot://kernel",
                        logs: {
                            last_timestamp: AnyProperty,
                            total: 3u64,
                            dropped: 0u64,
                            trace: 0u64,
                            debug: 0u64,
                            info: 3u64,
                            warn: 0u64,
                            error: 0u64,
                            fatal: 0u64,
                        },
                    },
                }
            }
        );
    }
}

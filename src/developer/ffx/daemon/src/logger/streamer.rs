// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context, Result},
    async_std::{
        fs::{create_dir_all, read_dir, remove_dir_all, remove_file, File, OpenOptions},
        io::{BufReader, Lines},
        path::PathBuf,
        prelude::*,
        stream::{Stream, StreamExt},
        sync::{Arc, RwLock, RwLockWriteGuard},
        task::Poll,
    },
    async_trait::async_trait,
    diagnostics_data::Timestamp,
    ffx_config::get,
    ffx_log_data::LogEntry,
    fidl_fuchsia_developer_bridge::StreamMode,
    futures::{FutureExt, TryStreamExt},
    std::convert::TryInto,
    std::io::ErrorKind,
    std::iter::Iterator,
    std::pin::Pin,
};

const CACHE_DIRECTORY_CONFIG: &str = "proactive_log.cache_directory";
const MAX_LOG_SIZE_CONFIG: &str = "proactive_log.max_log_size_bytes";
const MAX_SESSION_SIZE_CONFIG: &str = "proactive_log.max_session_size_bytes";
const MAX_SESSIONS_CONFIG: &str = "proactive_log.max_sessions_per_target";

struct LogFileEntries {
    lines: Lines<BufReader<Box<File>>>,
}

impl LogFileEntries {
    fn new(lines: Lines<BufReader<Box<File>>>) -> Self {
        Self { lines }
    }
}

impl Stream for LogFileEntries {
    type Item = Result<LogEntry>;

    fn poll_next(
        mut self: std::pin::Pin<&mut Self>,
        cx: &mut futures::task::Context<'_>,
    ) -> Poll<Option<Self::Item>> {
        Pin::new(&mut self.lines).poll_next(cx).map(|line_opt| {
            line_opt.map(|line| match line {
                Ok(line) => serde_json::from_str(&line).map_err(|e| anyhow!(e)),
                Err(e) => Err(anyhow!(e)),
            })
        })
    }
}

async fn sort_directory(parent: &PathBuf) -> Result<Vec<PathBuf>> {
    let mut reader = read_dir(parent.clone()).await?;
    let mut result = vec![];
    while let Some(ent) = reader.try_next().await? {
        let fname_num: u64 = match String::from(ent.file_name().to_string_lossy()).parse() {
            Ok(name) => name,
            Err(_) => continue,
        };
        result.push((fname_num, ent));
    }

    result.sort_by_key(|tup| tup.0);

    Ok(result.iter().map(|tup| tup.1.path()).collect())
}

#[derive(Debug)]
struct LogFile {
    path: PathBuf,
    parent: TargetSessionDirectory,
    file: Option<Box<File>>,
}

impl LogFile {
    fn new(path: PathBuf, parent: TargetSessionDirectory) -> Self {
        Self { path, parent, file: None }
    }

    fn from_file(path: PathBuf, parent: TargetSessionDirectory, file: Box<File>) -> Self {
        Self { path, parent, file: Some(file) }
    }

    async fn create(parent: TargetSessionDirectory) -> Result<Self> {
        let fname = match parent.latest_file().await? {
            Some(f) => f.parsed_name()? + 1,
            None => 1,
        };

        let mut file_path = parent.to_path_buf();
        file_path.push(fname.to_string());

        let mut options = OpenOptions::new();
        options.create(true).write(true);

        let f = options
            .open(file_path.clone())
            .await
            .context("opening output file")
            .map(|f| Box::new(f))?;

        Ok(Self::from_file(file_path, parent.clone(), f))
    }

    fn parsed_name(&self) -> Result<u64> {
        Ok(String::from(
            self.path
                .file_name()
                .ok_or(anyhow!("invalid path name {:?}", &self.path.to_str()))?
                .to_string_lossy(),
        )
        .parse()?)
    }

    async fn get_file(&mut self) -> Result<&mut Box<File>> {
        if self.file.is_none() {
            let mut options = OpenOptions::new();
            options.append(true);
            self.file = Some(
                options
                    .open(self.path.clone())
                    .await
                    .context("opening output file")
                    .map(|f| Box::new(f))?,
            );
        }

        Ok(self.file.as_mut().unwrap())
    }

    /// Utility function for immediately opening the underlying file
    /// for this LogFile.
    pub async fn force_open(&mut self) -> Result<()> {
        self.get_file().await.map(|_| ())
    }

    async fn bytes_in_file(&mut self) -> Result<usize> {
        Ok(self.get_file().await?.metadata().await?.len().try_into()?)
    }

    async fn remove(&mut self) -> Result<()> {
        remove_file(self.path.clone()).await?;
        self.file = None;
        Ok(())
    }

    async fn write_entries(&mut self, entries: Vec<&'_ Vec<u8>>) -> Result<()> {
        let f = self.get_file().await?;

        for entry in entries.iter() {
            f.write_all(entry.as_slice()).await?;
        }
        f.sync_data().await?;

        Ok(())
    }

    async fn stream_entries(&self) -> Result<LogFileEntries> {
        Ok(LogFileEntries::new(
            BufReader::new(Box::new(File::open(self.path.clone()).await?)).lines(),
        ))
    }
}

#[derive(Clone, Debug)]
pub struct TargetLogDirectory {
    root: PathBuf,
}

impl TargetLogDirectory {
    async fn new(nodename: String) -> Result<Self> {
        let mut root = get::<std::path::PathBuf, &str>(CACHE_DIRECTORY_CONFIG).await?;
        root.push(nodename);
        Ok(Self { root: root.into() })
    }

    #[cfg(test)]
    fn new_with_root(root_dir: PathBuf, nodename: String) -> Self {
        let mut root = root_dir.clone();
        root.push(nodename);
        Self { root }
    }

    fn with_session(&self, timestamp_millis: usize) -> TargetSessionDirectory {
        TargetSessionDirectory::new(self.clone(), timestamp_millis)
    }

    pub async fn clean_sessions(&self, max_sessions: usize) -> Result<()> {
        let entries = sort_directory(&self.root).await?;
        if entries.len() > max_sessions {
            let end = entries.len() - max_sessions;
            let entries_to_remove = &entries[..end];

            for path in entries_to_remove.iter() {
                log::info!("[logger] garbage collecting log directory: {:?}", &path);
                remove_dir_all(path).await?;
            }
        }

        Ok(())
    }

    fn to_path_buf(&self) -> PathBuf {
        self.root.clone()
    }
}

#[derive(Clone, Debug)]
struct TargetSessionDirectory {
    target_root: TargetLogDirectory,
    full_path: PathBuf,
}

impl TargetSessionDirectory {
    fn new(target_root: TargetLogDirectory, timestamp_millis: usize) -> Self {
        let mut pb = target_root.to_path_buf();
        pb.push(timestamp_millis.to_string());

        Self { target_root, full_path: pb }
    }

    async fn sort_entries(&self) -> Result<Vec<LogFile>> {
        Ok(sort_directory(&self.full_path)
            .await?
            .iter()
            .map(|p| LogFile::new(p.clone(), self.clone()))
            .collect())
    }

    fn parent(&self) -> TargetLogDirectory {
        self.target_root.clone()
    }

    async fn latest_file(&self) -> Result<Option<LogFile>> {
        self.sort_entries().await.map(|v| v.into_iter().rev().next())
    }

    async fn create_file(&self) -> Result<LogFile> {
        LogFile::create(self.clone()).await
    }

    fn to_path_buf(&self) -> PathBuf {
        self.full_path.clone()
    }
}

struct CachedSessionStream {
    file_iter: Option<LogFileEntries>,
    chunks: Vec<LogFile>,
    index: usize,
    end_timestamp: Option<Timestamp>,
    finished: bool,
}

impl CachedSessionStream {
    async fn new(
        session_dir: TargetSessionDirectory,
        end_timestamp: Option<Timestamp>,
        stream_mode: StreamMode,
    ) -> Result<Self> {
        let mut entries = session_dir.sort_entries().await?;
        if stream_mode == StreamMode::SnapshotRecentThenSubscribe && entries.len() > 2 {
            entries.drain(0..entries.len() - 2);
        }

        // Force every LogFile to cache its underlying FS file. This allows to continue
        // streaming data even if the files get garbage collected during iteration.
        futures::future::join_all(entries.iter_mut().map(|e| e.force_open())).await;

        Ok(Self { chunks: entries, index: 0, file_iter: None, finished: false, end_timestamp })
    }

    pub async fn iter(&mut self) -> Result<Option<Result<LogEntry>>> {
        if self.finished {
            return Ok(None);
        }
        loop {
            if self.file_iter.is_some() {
                let val = self.file_iter.as_mut().unwrap().next().await;
                if val.is_some() {
                    let entry = val.unwrap();
                    if !entry.is_ok() {
                        return Ok(Some(entry));
                    }

                    let entry = entry.unwrap();
                    if let Some(ts) = self.end_timestamp {
                        if entry.timestamp > ts {
                            self.finished = true;
                            return Ok(None);
                        }
                    }

                    return Ok(Some(Ok(entry)));
                } else {
                    self.file_iter = None;
                    self.index += 1;
                }
            }

            if let Some(chunk) = self.chunks.get(self.index) {
                let iterator = chunk.stream_entries().await?;
                self.file_iter = Some(iterator);
                continue;
            } else {
                self.finished = true;
                return Ok(None);
            }
        }
    }
}

pub struct SessionStream {
    cache_stream: CachedSessionStream,
    cache_read_finished: bool,

    // We hold onto the Sender in order to create the receiver only at the point
    // that we've finished iterating through the on-disk cache. This prevents
    // duplicating any log entries.
    read_stream: Arc<mpmc::Sender<LogEntry>>,
    read_receiver: Option<mpmc::Receiver<LogEntry>>,
    stream_mode: StreamMode,
}

impl SessionStream {
    async fn new(
        session_dir: TargetSessionDirectory,
        end_timestamp: Option<Timestamp>,
        stream_mode: StreamMode,
        read_stream: Arc<mpmc::Sender<LogEntry>>,
    ) -> Result<Self> {
        // In the case of a Subscribe only stream_mode, we can create the receiver immediately
        // because we have no risk of duplicating on-disk log entries.
        let receiver = if stream_mode == StreamMode::Subscribe {
            Some(read_stream.new_receiver())
        } else {
            None
        };
        Ok(Self {
            cache_stream: CachedSessionStream::new(session_dir, end_timestamp, stream_mode).await?,
            cache_read_finished: false,
            read_receiver: receiver,
            read_stream,
            stream_mode: stream_mode,
        })
    }

    pub async fn iter(&mut self) -> Result<Option<Result<LogEntry>>> {
        if self.stream_mode != StreamMode::Subscribe && !self.cache_read_finished {
            let res = self.cache_stream.iter().await;
            if let Ok(opt) = res {
                if opt.is_some() {
                    return Ok(opt);
                } else {
                    self.cache_read_finished = true;
                }
            } else {
                return res;
            }
        }

        if self.stream_mode != StreamMode::SnapshotAll {
            if self.read_receiver.is_none() {
                self.read_receiver = Some(self.read_stream.new_receiver());
            }
            let res = self
                .read_receiver
                .as_mut()
                .unwrap()
                .next()
                .map(|log_opt| Ok(log_opt.map(|l| Ok(l))))
                .await;

            return res;
        } else {
            return Ok(None);
        }
    }
}

#[derive(Default)]
struct DiagnosticsStreamerInner {
    output_dir: Option<TargetSessionDirectory>,
    current_file: Option<LogFile>,
    max_file_size_bytes: usize,
    max_session_size_bytes: usize,
    max_num_sessions: usize,
    read_stream: Arc<mpmc::Sender<LogEntry>>,
}

impl Clone for DiagnosticsStreamerInner {
    fn clone(&self) -> Self {
        Self {
            output_dir: self.output_dir.clone(),
            current_file: None,
            max_file_size_bytes: self.max_file_size_bytes,
            max_session_size_bytes: self.max_session_size_bytes,
            max_num_sessions: self.max_num_sessions,
            read_stream: self.read_stream.clone(),
        }
    }
}

pub struct DiagnosticsStreamer {
    inner: RwLock<DiagnosticsStreamerInner>,
}

impl Clone for DiagnosticsStreamer {
    fn clone(&self) -> Self {
        let inner = futures::executor::block_on(self.inner.read());
        Self { inner: RwLock::new(inner.clone()) }
    }
}

impl Default for DiagnosticsStreamer {
    fn default() -> Self {
        Self { inner: RwLock::new(DiagnosticsStreamerInner::default()) }
    }
}

#[async_trait]
pub trait GenericDiagnosticsStreamer {
    async fn setup_stream(
        &self,
        target_nodename: String,
        target_boot_time_nanos: u64,
    ) -> Result<()>;

    async fn append_logs(&self, entries: Vec<LogEntry>) -> Result<()>;

    async fn read_most_recent_timestamp(&self) -> Result<Option<Timestamp>>;

    async fn clean_sessions_for_target(&self) -> Result<()>;

    async fn stream_entries(&self, stream_mode: StreamMode) -> Result<SessionStream>;
}

#[async_trait::async_trait]
impl GenericDiagnosticsStreamer for DiagnosticsStreamer {
    async fn setup_stream(
        &self,
        target_nodename: String,
        target_boot_time_nanos: u64,
    ) -> Result<()> {
        self.setup_stream_with_config(
            TargetLogDirectory::new(target_nodename).await?,
            target_boot_time_nanos,
            get::<u64, &str>(MAX_LOG_SIZE_CONFIG).await? as usize,
            get::<u64, &str>(MAX_SESSION_SIZE_CONFIG).await? as usize,
            get::<u64, &str>(MAX_SESSIONS_CONFIG).await? as usize,
        )
        .await
    }

    async fn append_logs(&self, raw_entries: Vec<LogEntry>) -> Result<()> {
        let entries = raw_entries
            .iter()
            .filter_map(|entry| match serde_json::to_string(entry) {
                Ok(s) => {
                    let mut owned = s.clone();
                    owned.push('\n');
                    Some(owned.into_bytes())
                }
                Err(e) => {
                    log::warn!("failed to serialize LogEntry: {:?}. Log was: {:?}", e, &entry);
                    None
                }
            })
            .collect::<Vec<_>>();

        let mut inner = self.inner.write().await;
        let max_file_size = inner.max_file_size_bytes;
        let parent = inner.output_dir.as_ref().context("no stream setup")?.clone();

        let mut file = match inner.current_file.take() {
            Some(f) => f,
            None => {
                // Continue appending to the latest file for this session if one exists.
                let latest_file = parent.latest_file().await?;

                match latest_file {
                    Some(f) => f,
                    None => parent.create_file().await?,
                }
            }
        };

        let mut buf_bytes = file.bytes_in_file().await?;
        let mut buf: Vec<&Vec<u8>> = vec![];

        for log_bytes in entries.as_slice() {
            // We flush the buffer to disk if this log entry would push us over the max file size
            // or if this log entry is, alone, larger the max file size (rather than discard the log entry)
            if !buf.is_empty()
                && (buf_bytes + log_bytes.len() > max_file_size || log_bytes.len() > max_file_size)
            {
                file.write_entries(buf).await?;
                buf_bytes = 0;
                buf = vec![];

                file = parent.create_file().await?;
            }

            buf_bytes += log_bytes.len();
            buf.push(log_bytes);
        }

        if !buf.is_empty() {
            // We don't need to create a new file here in the special case that
            // the buffer exceeds the max file size, but hasn't been
            // flushed yet (i.e. we got a single log entry > max_file_size)
            if buf_bytes > max_file_size && file.bytes_in_file().await? != 0 {
                file = parent.create_file().await?;
            }
            file.write_entries(buf).await?;
        }

        inner.current_file = Some(file);

        for entry in raw_entries.iter() {
            inner.read_stream.send(entry.clone()).await;
        }

        self.cleanup_logs(inner).await?;

        Ok(())
    }

    async fn read_most_recent_timestamp(&self) -> Result<Option<Timestamp>> {
        let inner = self.inner.read().await;
        let output_dir = inner.output_dir.as_ref().context("stream not setup")?;

        let files = output_dir.sort_entries().await?;
        for file in files.iter().rev() {
            let entry = file
                .stream_entries()
                .await?
                .filter_map(|l| l.ok())
                .map(|l| l.timestamp)
                .last()
                .await;
            if entry.is_some() {
                return Ok(entry);
            }
        }
        Ok(None)
    }

    async fn clean_sessions_for_target(&self) -> Result<()> {
        let inner = self.inner.read().await;
        inner
            .output_dir
            .as_ref()
            .context("missing output directory")?
            .parent()
            .clean_sessions(inner.max_num_sessions)
            .await
    }

    async fn stream_entries(&self, stream_mode: StreamMode) -> Result<SessionStream> {
        let ts = if stream_mode == StreamMode::SnapshotAll {
            Some(self.read_most_recent_timestamp().await?.unwrap_or(Timestamp::from(0u64)))
        } else {
            None
        };

        let inner = self.inner.read().await;
        let output_dir = inner.output_dir.as_ref().context("stream not setup")?;

        SessionStream::new(output_dir.clone(), ts, stream_mode, inner.read_stream.clone()).await
    }
}

impl DiagnosticsStreamer {
    // This should only be called by tests.
    pub(crate) async fn setup_stream_with_config(
        &self,
        target_root_dir: TargetLogDirectory,
        target_boot_time_nanos: u64,
        max_file_size_bytes: usize,
        max_session_size_bytes: usize,
        max_num_sessions: usize,
    ) -> Result<()> {
        // The ticks=>time conversion isn't accurate enough to use units smaller than milliseconds here.
        let t = target_boot_time_nanos / 1_000_000;

        let session_dir = target_root_dir.with_session(t as usize);

        create_dir_all(session_dir.to_path_buf()).await.or_else(|e| match e.kind() {
            ErrorKind::AlreadyExists => Ok(()),
            _ => Err(e),
        })?;

        let mut inner = self.inner.write().await;
        inner.output_dir.replace(session_dir);
        inner.max_file_size_bytes = max_file_size_bytes;
        inner.max_session_size_bytes = max_session_size_bytes;
        inner.max_num_sessions = max_num_sessions;
        Ok(())
    }

    async fn cleanup_logs(
        &self,
        inner: RwLockWriteGuard<'_, DiagnosticsStreamerInner>,
    ) -> Result<()> {
        let output_dir = inner.output_dir.as_ref().context("no stream setup")?;
        let mut entries = output_dir.sort_entries().await?;

        // We approximate the need to garbage collect by multiplying the number of files by
        // the max file size, *excluding* the most recent file.
        if entries.len() > 1
            && (entries.len() - 1) * inner.max_file_size_bytes > inner.max_session_size_bytes
        {
            let to_remove = entries.first_mut().unwrap();
            log::info!("logger: garbage collecting log file: {:?}", to_remove);
            to_remove.remove().await?;
        }

        Ok(())
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        async_std::{fs::read_to_string, future::timeout},
        diagnostics_data::{LogsData, LogsField, Severity},
        diagnostics_hierarchy::{DiagnosticsHierarchy, Property},
        ffx_log_data::LogData,
        std::collections::HashMap,
        std::time::Duration,
        tempfile::{tempdir, TempDir},
    };

    const FAKE_DIR_NAME: &str = "fake_logs";
    const SMALL_MAX_LOG_SIZE: usize = 10;
    const LARGE_MAX_LOG_SIZE: usize = 1_000_000;
    const SMALL_MAX_SESSION_SIZE: usize = 11;
    const LARGE_MAX_SESSION_SIZE: usize = 1_000_001;
    const DEFAULT_MAX_SESSIONS: usize = 1;
    const NODENAME: &str = "my-cool-node";
    const BOOT_TIME_NANOS: u64 = 123456789000000000;
    const BOOT_TIME_MILLIS: u64 = 123456789000;
    const TIMESTAMP: u64 = 987654321;
    const READ_TIMEOUT_MILLIS: u64 = 100;

    async fn collect_logs(path: PathBuf) -> Result<HashMap<String, String>> {
        let mut result = HashMap::new();
        let mut ent_stream = read_dir(path).await?;
        while let Some(f) = ent_stream.next().await {
            let f = f?;
            result.insert(
                String::from(f.file_name().to_string_lossy()),
                read_to_string(String::from(f.path().to_str().unwrap())).await?,
            );
        }

        Ok(result)
    }

    async fn collect_default_logs(dir_path: &PathBuf) -> Result<HashMap<String, String>> {
        collect_default_logs_for_session(dir_path, BOOT_TIME_MILLIS).await
    }

    async fn collect_default_logs_for_session(
        dir_path: &PathBuf,
        boot_time_millis: u64,
    ) -> Result<HashMap<String, String>> {
        let mut root = dir_path.clone();
        root.push(FAKE_DIR_NAME);
        root.push(NODENAME);
        root.push(boot_time_millis.to_string());
        collect_logs(root).await
    }

    fn make_target_log(msg: String) -> LogsData {
        let hierarchy =
            DiagnosticsHierarchy::new("root", vec![Property::String(LogsField::Msg, msg)], vec![]);
        LogsData::for_logs(
            String::from("test/moniker"),
            Some(hierarchy),
            Timestamp::from(0u64),
            String::from("fake-url"),
            Severity::Error,
            1,
            vec![],
        )
    }

    fn make_malformed_log(ts: u64) -> LogEntry {
        LogEntry {
            data: LogData::MalformedTargetLog("fake log data".to_string()),
            version: 1,
            timestamp: ts.into(),
        }
    }

    fn make_valid_log(ts: u64, msg: String) -> LogEntry {
        LogEntry {
            data: LogData::TargetLog(make_target_log(msg)),
            version: 1,
            timestamp: ts.into(),
        }
    }

    async fn setup_default_streamer_with_temp_and_boot_time(
        temp_parent: &TempDir,
        boot_time_nanos: u64,
        max_log_size: usize,
        max_session_size: usize,
        max_sessions: usize,
    ) -> Result<DiagnosticsStreamer> {
        let mut root: PathBuf = temp_parent.path().to_path_buf().into();
        root.push(FAKE_DIR_NAME.to_string());

        let streamer = DiagnosticsStreamer::default();
        let target_dir = TargetLogDirectory::new_with_root(root.clone(), NODENAME.to_string());
        streamer
            .setup_stream_with_config(
                target_dir,
                boot_time_nanos,
                max_log_size,
                max_session_size,
                max_sessions,
            )
            .await?;

        Ok(streamer)
    }

    async fn setup_default_streamer_with_temp(
        temp_parent: &TempDir,
        max_log_size: usize,
        max_session_size: usize,
        max_sessions: usize,
    ) -> Result<DiagnosticsStreamer> {
        setup_default_streamer_with_temp_and_boot_time(
            temp_parent,
            BOOT_TIME_NANOS,
            max_log_size,
            max_session_size,
            max_sessions,
        )
        .await
    }

    async fn setup_default_streamer(
        max_log_size: usize,
        max_session_size: usize,
        max_sessions: usize,
    ) -> Result<(TempDir, DiagnosticsStreamer)> {
        let temp_parent = tempdir()?;
        let streamer = setup_default_streamer_with_temp(
            &temp_parent,
            max_log_size,
            max_session_size,
            max_sessions,
        )
        .await?;

        Ok((temp_parent, streamer))
    }

    async fn verify_logs(mut reader: SessionStream, logs: Vec<LogEntry>) {
        for log in logs.iter() {
            let item = reader.iter().await.unwrap();
            assert!(
                item.is_some(),
                "expected log {:?} has no corresponding entry in log file",
                log
            );
            assert_eq!(item.unwrap().unwrap(), *log);
        }

        assert!(reader.iter().await.unwrap().is_none());
    }

    async fn verify_log(reader: &mut SessionStream, expected: LogEntry) -> Result<()> {
        assert_eq!(
            timeout(Duration::from_millis(READ_TIMEOUT_MILLIS), reader.iter())
                .await??
                .context("missing log entry")??,
            expected
        );
        Ok(())
    }

    async fn verify_times_out(reader: &mut SessionStream) {
        let res = timeout(Duration::from_millis(READ_TIMEOUT_MILLIS), reader.iter()).await;
        assert!(res.is_err(), "expected timeout, got {:?}", res);
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_single_log_exceeds_max_size() -> Result<()> {
        let (temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;
        let root: PathBuf = temp.path().to_path_buf().into();
        let log = make_valid_log(TIMESTAMP, "log".to_string());
        streamer.append_logs(vec![log.clone()]).await?;

        let results = collect_default_logs(&root).await.unwrap();
        assert_eq!(results.len(), 1, "{:?}", results);
        assert_eq!(
            serde_json::from_str::<LogEntry>(results.values().next().unwrap()).unwrap(),
            log
        );

        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log]).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_two_logs_exceeds_max_size() -> Result<()> {
        let (temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;
        let root: PathBuf = temp.path().to_path_buf().into();
        let log = make_valid_log(TIMESTAMP, "log".to_string());
        let log2 = make_valid_log(TIMESTAMP, "log2".to_string());
        streamer.append_logs(vec![log.clone(), log2.clone()]).await?;

        let results = collect_default_logs(&root).await.unwrap();
        assert_eq!(results.len(), 2, "{:?}", results);
        let mut values = results.values().collect::<Vec<_>>();
        values.sort();
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(0).unwrap()).unwrap(), log);
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(1).unwrap()).unwrap(), log2);

        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log, log2]).await;
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_two_logs_with_two_calls_exceeds_max_size() -> Result<()> {
        let (temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            SMALL_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;
        let root: PathBuf = temp.path().to_path_buf().into();
        let log = make_valid_log(TIMESTAMP, "log".to_string());
        let log2 = make_valid_log(TIMESTAMP, "log2".to_string());
        streamer.append_logs(vec![log.clone()]).await?;
        streamer.append_logs(vec![log2.clone()]).await?;

        let results = collect_default_logs(&root).await.unwrap();
        assert_eq!(results.len(), 2, "{:?}", results);
        let mut values = results.values().collect::<Vec<_>>();
        values.sort();
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(0).unwrap()).unwrap(), log);
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(1).unwrap()).unwrap(), log2);

        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log, log2]).await;
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_garbage_collection_doesnt_count_latest_file() -> Result<()> {
        let (temp, streamer) =
            setup_default_streamer(SMALL_MAX_LOG_SIZE, SMALL_MAX_LOG_SIZE, DEFAULT_MAX_SESSIONS)
                .await?;
        let root: PathBuf = temp.path().to_path_buf().into();
        let log = make_valid_log(TIMESTAMP, "log".to_string());
        let log2 = make_valid_log(TIMESTAMP, "log2".to_string());
        streamer.append_logs(vec![log.clone()]).await?;
        streamer.append_logs(vec![log2.clone()]).await?;

        let results = collect_default_logs(&root).await.unwrap();
        assert_eq!(results.len(), 2, "{:?}", results);
        let mut values = results.values().collect::<Vec<_>>();
        values.sort();
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(0).unwrap()).unwrap(), log);
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(1).unwrap()).unwrap(), log2);

        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log, log2]).await;
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_two_logs_with_two_calls_exceeds_max_session_size() -> Result<()> {
        let (temp, streamer) =
            setup_default_streamer(SMALL_MAX_LOG_SIZE, SMALL_MAX_LOG_SIZE, DEFAULT_MAX_SESSIONS)
                .await?;
        let root: PathBuf = temp.path().to_path_buf().into();
        let log = make_valid_log(TIMESTAMP, "log".to_string());
        let log2 = make_valid_log(TIMESTAMP, "log2".to_string());
        let log3 = make_valid_log(TIMESTAMP, "log3".to_string());
        streamer.append_logs(vec![log.clone()]).await?;
        streamer.append_logs(vec![log2.clone()]).await?;
        // This call should delete the first log file.
        streamer.append_logs(vec![log3.clone()]).await?;

        let results = collect_default_logs(&root).await.unwrap();
        assert_eq!(results.len(), 2, "{:?}", results);
        let mut values = results.values().collect::<Vec<_>>();
        values.sort();
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(0).unwrap()).unwrap(), log2);
        assert_eq!(serde_json::from_str::<LogEntry>(values.get(1).unwrap()).unwrap(), log3);

        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log2, log3])
            .await;
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_new_streamer_appends_to_existing_file() -> Result<()> {
        let temp = tempdir()?;
        let root: PathBuf = temp.path().to_path_buf().into();
        let log = make_valid_log(TIMESTAMP, "log1".to_string());
        let log2 = make_valid_log(TIMESTAMP + 1, "log2".to_string());
        {
            let streamer = setup_default_streamer_with_temp(
                &temp,
                LARGE_MAX_LOG_SIZE,
                LARGE_MAX_SESSION_SIZE,
                DEFAULT_MAX_SESSIONS,
            )
            .await
            .unwrap();
            streamer.append_logs(vec![log.clone()]).await.unwrap();
        }

        let streamer = setup_default_streamer_with_temp(
            &temp,
            LARGE_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await
        .unwrap();
        streamer.append_logs(vec![log2.clone()]).await.unwrap();

        let results = collect_default_logs(&root).await.unwrap();
        assert_eq!(results.len(), 1, "{:?}", results);
        let value = results.values().next().unwrap();

        let results = value.split("\n").collect::<Vec<_>>();
        assert_eq!(results.len(), 3, "{:?}", results);

        assert_eq!(serde_json::from_str::<LogEntry>(results.get(0).unwrap()).unwrap(), log);
        assert_eq!(serde_json::from_str::<LogEntry>(results.get(1).unwrap()).unwrap(), log2);
        assert!(results.get(2).unwrap().is_empty());

        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log, log2]).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_snapshot_does_not_include_later_writes() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;
        let log = make_valid_log(TIMESTAMP, "log1".to_string());
        streamer.append_logs(vec![log.clone()]).await?;

        let iterator = streamer.stream_entries(StreamMode::SnapshotAll).await?;

        let log2 = make_valid_log(TIMESTAMP + 1, "log2".to_string());
        streamer.append_logs(vec![log2.clone()]).await?;

        verify_logs(iterator, vec![log.clone()]).await;
        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log, log2]).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_snapshot_returns_nothing_if_no_logs_at_write_time() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;
        let mut iterator = streamer.stream_entries(StreamMode::SnapshotAll).await?;

        let log = make_valid_log(TIMESTAMP, "log1".to_string());
        streamer.append_logs(vec![log.clone()]).await?;

        assert!(iterator.iter().await?.is_none());
        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log]).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_snapshot_subscribe_intermingled_writes_all_disk() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            LARGE_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;

        let log1 = make_valid_log(TIMESTAMP, "log1".to_string());
        let log2 = make_valid_log(TIMESTAMP + 1, "log2".to_string());
        let log3 = make_valid_log(TIMESTAMP + 2, "log3".to_string());
        let log4 = make_valid_log(TIMESTAMP + 3, "log4".to_string());
        streamer.append_logs(vec![log1.clone(), log2.clone()]).await?;

        let mut iterator = streamer.stream_entries(StreamMode::SnapshotAllThenSubscribe).await?;
        verify_log(&mut iterator, log1.clone()).await.context(format!("{:?}", log1)).unwrap();

        streamer.append_logs(vec![log3.clone()]).await?;
        verify_log(&mut iterator, log2.clone()).await.context(format!("{:?}", log2)).unwrap();
        verify_log(&mut iterator, log3.clone()).await.context(format!("{:?}", log3)).unwrap();

        streamer.append_logs(vec![log4.clone()]).await?;
        verify_log(&mut iterator, log4.clone()).await.context(format!("{:?}", log4)).unwrap();
        verify_times_out(&mut iterator).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_snapshot_subscribe_intermingled_writes() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            LARGE_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;

        let log1 = make_valid_log(TIMESTAMP, "log1".to_string());
        let log2 = make_valid_log(TIMESTAMP + 1, "log2".to_string());
        let log3 = make_valid_log(TIMESTAMP + 2, "log3".to_string());
        let log4 = make_valid_log(TIMESTAMP + 3, "log4".to_string());
        streamer.append_logs(vec![log1.clone(), log2.clone()]).await?;

        let mut iterator = streamer.stream_entries(StreamMode::SnapshotAllThenSubscribe).await?;
        verify_log(&mut iterator, log1.clone()).await.context(format!("{:?}", log1)).unwrap();
        verify_log(&mut iterator, log2.clone()).await.context(format!("{:?}", log2)).unwrap();
        verify_times_out(&mut iterator).await;

        streamer.append_logs(vec![log3.clone()]).await?;
        verify_log(&mut iterator, log3.clone()).await.context(format!("{:?}", log3)).unwrap();

        streamer.append_logs(vec![log4.clone()]).await?;
        verify_log(&mut iterator, log4.clone()).await.context(format!("{:?}", log4)).unwrap();
        verify_times_out(&mut iterator).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_snapshot_recent_includes_only_recent() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;
        let log = make_valid_log(TIMESTAMP, "should not be read".to_string());
        let log2 = make_valid_log(TIMESTAMP, "log2".to_string());
        let log3 = make_valid_log(TIMESTAMP, "log3".to_string());
        let log4 = make_valid_log(TIMESTAMP, "log4".to_string());
        streamer.append_logs(vec![log.clone(), log.clone(), log2.clone(), log3.clone()]).await?;

        // SnapshotRecent will include only the most recent two chunks
        let mut iterator = streamer.stream_entries(StreamMode::SnapshotRecentThenSubscribe).await?;
        verify_log(&mut iterator, log2.clone()).await.context(format!("{:?}", log2)).unwrap();
        verify_log(&mut iterator, log3.clone()).await.context(format!("{:?}", log3)).unwrap();
        verify_times_out(&mut iterator).await;

        streamer.append_logs(vec![log4.clone()]).await?;
        verify_log(&mut iterator, log4.clone()).await.context(format!("{:?}", log4)).unwrap();
        verify_times_out(&mut iterator).await;

        let mut iterator = streamer.stream_entries(StreamMode::SnapshotRecentThenSubscribe).await?;
        verify_log(&mut iterator, log3.clone()).await.context(format!("{:?}", log3)).unwrap();
        verify_log(&mut iterator, log4.clone()).await.context(format!("{:?}", log4)).unwrap();
        verify_times_out(&mut iterator).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_subscribe_ignores_on_disk_logs() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            LARGE_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;

        let log1 = make_valid_log(TIMESTAMP, "log1".to_string());
        let log2 = make_valid_log(TIMESTAMP + 1, "log2".to_string());
        let log3 = make_valid_log(TIMESTAMP + 2, "log3".to_string());
        let log4 = make_valid_log(TIMESTAMP + 3, "log4".to_string());
        streamer.append_logs(vec![log1.clone(), log2.clone()]).await?;

        let mut iterator = streamer.stream_entries(StreamMode::Subscribe).await?;
        streamer.append_logs(vec![log3.clone()]).await?;
        verify_log(&mut iterator, log3.clone()).await.context(format!("{:?}", log3)).unwrap();

        streamer.append_logs(vec![log4.clone()]).await?;
        verify_log(&mut iterator, log4.clone()).await.context(format!("{:?}", log4)).unwrap();
        verify_times_out(&mut iterator).await;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_write_no_setup_call_errors() -> Result<()> {
        let streamer = DiagnosticsStreamer::default();
        let log = make_malformed_log(TIMESTAMP);
        assert!(streamer.append_logs(vec![log]).await.is_err());

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_read_no_setup_call_errors() -> Result<()> {
        let streamer = DiagnosticsStreamer::default();
        assert!(streamer.stream_entries(StreamMode::SnapshotAll).await.is_err());

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_simple_read() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;

        let early_log = make_valid_log(TIMESTAMP - 1, String::default());
        let log = make_valid_log(TIMESTAMP, String::default());

        streamer.append_logs(vec![early_log, log]).await?;

        assert_eq!(streamer.read_most_recent_timestamp().await?.unwrap(), TIMESTAMP.into());
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_read_timestamp_returns_none_if_no_logs() -> Result<()> {
        let (_temp, streamer) = setup_default_streamer(
            SMALL_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await?;

        assert!(streamer.read_most_recent_timestamp().await?.is_none());
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_cleans_old_sessions() -> Result<()> {
        let temp = tempdir()?;
        let root: PathBuf = temp.path().to_path_buf().into();

        let log = make_valid_log(TIMESTAMP, "log".to_string());
        let log2 = make_valid_log(TIMESTAMP, "log2".to_string());
        let log3 = make_valid_log(TIMESTAMP, "log3".to_string());
        {
            let streamer = setup_default_streamer_with_temp_and_boot_time(
                &temp,
                BOOT_TIME_NANOS,
                LARGE_MAX_LOG_SIZE,
                LARGE_MAX_SESSION_SIZE,
                DEFAULT_MAX_SESSIONS,
            )
            .await
            .unwrap();
            streamer.append_logs(vec![log.clone()]).await.unwrap();
        }

        {
            let streamer = setup_default_streamer_with_temp_and_boot_time(
                &temp,
                BOOT_TIME_NANOS + 2_000_000,
                LARGE_MAX_LOG_SIZE,
                LARGE_MAX_SESSION_SIZE,
                DEFAULT_MAX_SESSIONS,
            )
            .await
            .unwrap();
            streamer.append_logs(vec![log2.clone()]).await.unwrap();
        }

        let streamer = setup_default_streamer_with_temp_and_boot_time(
            &temp,
            BOOT_TIME_NANOS + 3_000_000,
            LARGE_MAX_LOG_SIZE,
            LARGE_MAX_SESSION_SIZE,
            DEFAULT_MAX_SESSIONS,
        )
        .await
        .unwrap();
        streamer.append_logs(vec![log3.clone()]).await.unwrap();

        streamer.clean_sessions_for_target().await?;

        let mut session1 = root.clone();
        session1.push(FAKE_DIR_NAME);
        session1.push(NODENAME);
        session1.push(BOOT_TIME_MILLIS.to_string());
        assert!(!session1.exists().await);

        let mut session2 = root.clone();
        session2.push(FAKE_DIR_NAME);
        session2.push(NODENAME);
        session2.push((BOOT_TIME_MILLIS + 2).to_string());
        assert!(!session2.exists().await);

        let results = collect_default_logs_for_session(&root, BOOT_TIME_MILLIS + 3).await.unwrap();
        assert_eq!(results.len(), 1, "{:?}", results);
        let mut values = results.values().collect::<Vec<_>>();
        values.sort();
        assert_eq!(
            serde_json::from_str::<LogEntry>(values.get(0).unwrap()).unwrap(),
            log3,
            "{:?}",
            results
        );
        verify_logs(streamer.stream_entries(StreamMode::SnapshotAll).await?, vec![log3]).await;
        Ok(())
    }
}

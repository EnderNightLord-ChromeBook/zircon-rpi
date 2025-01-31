// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Result},
    chrono::prelude::*,
    std::{
        fs::File,
        io::{copy, Write},
        path::PathBuf,
    },
    zip::{
        write::{FileOptions, ZipWriter},
        CompressionMethod,
    },
};

const DOCTOR_OUTPUT_NAME: &str = "doctor_output.txt";

pub trait Recorder {
    fn add_sources(&mut self, source_files: Vec<PathBuf>);

    fn add_output(&mut self, s: String);

    fn generate(&self, output_dir: PathBuf) -> Result<PathBuf>;
}
pub struct DoctorRecorder {
    source_files: Vec<PathBuf>,
    doctor_output: String,
}

impl DoctorRecorder {
    pub fn new() -> Self {
        Self { source_files: vec![], doctor_output: String::new() }
    }

    fn copy_file(&self, source: &PathBuf, zip: &mut ZipWriter<File>) -> Result<()> {
        let mut f = File::open(source)?;
        let source_name = source.file_name().unwrap().to_string_lossy();
        zip.start_file(
            source_name,
            FileOptions::default().compression_method(CompressionMethod::Stored),
        )?;
        copy(&mut f, zip).map(|_| {}).map_err(|e| anyhow!(e))
    }
}

impl Recorder for DoctorRecorder {
    fn add_sources(&mut self, source_files: Vec<PathBuf>) {
        self.source_files.extend(source_files.into_iter());
    }

    fn add_output(&mut self, s: String) {
        self.doctor_output.push_str(&s);
    }

    fn generate(&self, output_dir: PathBuf) -> Result<PathBuf> {
        let fname = format!("ffx-record-{}.zip", Local::now().format("%Y%m%d-%H%M%S"));
        let mut output_path = output_dir.clone();
        output_path.push(fname);
        let file = File::create(output_path.clone())?;

        let mut zip = ZipWriter::new(file);
        for source in self.source_files.iter() {
            match self.copy_file(&source, &mut zip) {
                Ok(()) => {}
                Err(e) => {
                    log::warn!(
                        "unable to add '{}' to report zip: {:?}",
                        source.file_name().and_then(std::ffi::OsStr::to_str).unwrap(),
                        e
                    );
                    continue;
                }
            }
        }

        zip.start_file(
            DOCTOR_OUTPUT_NAME,
            FileOptions::default().compression_method(CompressionMethod::Stored),
        )?;
        zip.write(self.doctor_output.as_bytes()).map_err(|e| anyhow!(e))?;
        zip.finish().map_err(|e| anyhow!(e))?;

        Ok(output_path)
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        fuchsia_async as fasync,
        std::{
            collections::HashSet,
            fs::File,
            io::{Read, Write},
        },
        tempfile::tempdir,
        zip::read::ZipArchive,
    };

    const FAKE_OUTPUT: &str = "doctor doctor";
    const FILE_CONTENTS: &[u8] = b"fake log file";
    const FILE_NAME: &str = "log.txt";
    const NON_EXISTENT_FILE_NAME: &str = "does_not_exist.txt";

    #[fasync::run_singlethreaded(test)]
    async fn test() -> Result<()> {
        let temp = tempdir()?;
        let root = temp.path().to_path_buf();

        let mut f1path = root.clone();
        f1path.push(FILE_NAME);
        let mut f1 = File::create(f1path.clone())?;
        f1.write_all(FILE_CONTENTS)?;

        // This file won't actually exist on the filesystem. We'll
        // pass it to `add_sources` below to verify that `generate` still
        // succeeds.
        let mut f2path = root.clone();
        f2path.push(NON_EXISTENT_FILE_NAME);

        let mut rec = DoctorRecorder::new();
        rec.add_sources(vec![f1path, f2path]);
        rec.add_output(FAKE_OUTPUT.to_string());

        let path = rec.generate(root)?;
        let mut zip = ZipArchive::new(File::open(path)?)?;

        let actual_names: HashSet<_> = zip.file_names().collect();
        let expected_names: HashSet<_> = vec![DOCTOR_OUTPUT_NAME, FILE_NAME].into_iter().collect();
        assert_eq!(actual_names, expected_names);

        {
            let mut f1 = zip.by_name(FILE_NAME)?;
            let mut buf = String::new();
            f1.read_to_string(&mut buf)?;
            assert_eq!(buf.into_bytes(), FILE_CONTENTS);
        }

        let mut output = zip.by_name(DOCTOR_OUTPUT_NAME)?;
        let mut buf = String::new();
        output.read_to_string(&mut buf)?;
        assert_eq!(buf, FAKE_OUTPUT.to_string());

        Ok(())
    }
}

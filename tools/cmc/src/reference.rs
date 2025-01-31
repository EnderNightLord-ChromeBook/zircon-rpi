// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{error::Error, util},
    serde_json,
    std::{fs, path::PathBuf},
};

// These runners don't respect `program.binary`. We ignore validation for
// components using them.
// TODO(fxbug.dev/67151)
const RUNNER_IGNORE_LIST: [&'static str; 4] = [
    "fuchsia-pkg://fuchsia.com/guest-runner#meta/guest_runner.cmx",
    "fuchsia-pkg://fuchsia.com/appmgr_mock_runner#meta/appmgr_mock_runner.cmx",
    "fuchsia-pkg://fuchsia.com/netemul-runner#meta/netemul-runner.cmx",
    // TODO(fxbug.dev/68608): Add support for Dart components.
    "fuchsia-pkg://fuchsia.com/dart_jit_runner#meta/dart_jit_runner.cmx",
];

enum ComponentManifest {
    Cml(cml::Document),
    Cmx(serde_json::Value),
}

/// Validates that a component manifest file is consistent with the content
/// of its package. Checks included in this function are:
///     1. If provided program binary in component manifest matches with
///        executable target declared in package manifest.
/// If all checks pass, this function returns Ok(()).
pub fn validate(
    component_manifest_path: &PathBuf,
    package_manifest_path: &PathBuf,
    gn_label: Option<&String>,
) -> Result<(), Error> {
    let component_manifest = read_component_manifest(component_manifest_path)?;
    match get_component_runner(&component_manifest) {
        Some(component_runner) => {
            for runner in RUNNER_IGNORE_LIST.iter() {
                if &component_runner == runner {
                    return Ok(());
                }
            }
        }
        None => {}
    }

    let package_manifest = read_package_manifest(package_manifest_path)?;
    let package_targets = get_package_targets(&package_manifest);
    let program_binary = get_program_binary(&component_manifest);

    if program_binary.is_none() {
        return Ok(());
    }

    let program_binary = program_binary.unwrap();

    if package_targets.contains(&program_binary) {
        return Ok(());
    }

    // Legacy package.gni supports the "disabled test" feature that
    // intentionally breaks component manifests.
    if program_binary.starts_with("test/")
        && package_targets.contains(&format!("test/disabled/{}", &program_binary[5..]))
    {
        return Ok(());
    }

    Err(missing_binary_error(component_manifest_path, program_binary, package_targets, gn_label))
}

fn read_package_manifest(path: &PathBuf) -> Result<String, Error> {
    fs::read_to_string(path).map_err(|e| {
        Error::parse(format!("Couldn't parse file {:?}: {}", path, e), None, Some(path))
    })
}

fn read_component_manifest(path: &PathBuf) -> Result<ComponentManifest, Error> {
    const BAD_EXTENSION: &str = "Input file does not have a component manifest extension \
                                 (.cml or .cmx)";
    let ext = path.extension().and_then(|e| e.to_str());
    Ok(match ext {
        Some("cmx") => ComponentManifest::Cmx(util::read_cmx(path)?),
        Some("cml") => ComponentManifest::Cml(util::read_cml(path)?),
        _ => {
            return Err(Error::invalid_args(BAD_EXTENSION));
        }
    })
}

fn get_package_targets(package_manifest: &str) -> Vec<String> {
    package_manifest
        .lines()
        .map(|line| line.split("="))
        .map(|mut parts| parts.next())
        .filter(Option::is_some)
        // Safe to unwrap because Option(None) values have been filtered out.
        .map(Option::unwrap)
        .map(str::to_owned)
        .collect()
}

fn get_program_binary(component_manifest: &ComponentManifest) -> Option<String> {
    let program = match component_manifest {
        ComponentManifest::Cml(document) => match &document.program {
            Some(program) => &program.info,
            None => {
                return None;
            }
        },
        ComponentManifest::Cmx(body) => match body.get("program") {
            Some(program) => match program.as_object() {
                Some(program) => program,
                None => {
                    return None;
                }
            },
            None => {
                return None;
            }
        },
    };

    match program.get("binary") {
        Some(binary) => match binary.as_str() {
            Some(value) => Some(value.to_owned()),
            None => None,
        },
        None => None,
    }
}

fn get_component_runner(component_manifest: &ComponentManifest) -> Option<String> {
    match component_manifest {
        ComponentManifest::Cml(document) => {
            document.program.as_ref().and_then(|p| p.runner.as_ref().map(|s| s.as_str().to_owned()))
        }
        ComponentManifest::Cmx(payload) => {
            payload.get("runner").and_then(|r| r.as_str().map(|s| s.to_owned()))
        }
    }
}

fn missing_binary_error(
    component_manifest_path: &PathBuf,
    program_binary: String,
    package_targets: Vec<String>,
    gn_label: Option<&String>,
) -> Error {
    let header = gen_header(component_manifest_path, gn_label);
    if package_targets.is_empty() {
        return Error::validate(format!("{}\n\tPackage deps is empty!", header));
    }

    // We couldn't find the binary, let's get the nearest match.
    let nearest_match = get_nearest_match(&program_binary, &package_targets);

    Error::validate(format!(
        r"{}
    program.binary={} but '{}' is not provided by deps!

    Did you mean {:?}?

    Try any of the following:
    {:?}",
        header, program_binary, program_binary, nearest_match, package_targets
    ))
}

fn gen_header(component_manifest_path: &PathBuf, gn_label: Option<&String>) -> String {
    match gn_label {
        Some(label) => format!(
            "Error found in: {}\n\tFailed to validate manifest: {:#?}",
            label, component_manifest_path
        ),
        None => format!("Failed to validate manifest: {:#?}", component_manifest_path),
    }
}

fn get_nearest_match<'a>(reference: &'a str, candidates: &'a Vec<String>) -> &'a str {
    let mut nearest = &candidates[0];
    let mut min_distance = strsim::levenshtein(reference, &candidates[0]);
    for candidate in candidates.iter().skip(1) {
        let distance = strsim::levenshtein(reference, candidate);
        if distance < min_distance {
            min_distance = distance;
            nearest = candidate;
        }
    }
    &nearest
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        matches::assert_matches,
        serde_json::json,
        std::{fmt::Display, fs::File, io::Write},
        tempfile::TempDir,
    };

    fn tmp_file(tmp_dir: &TempDir, name: &str, contents: impl Display) -> PathBuf {
        let path = tmp_dir.path().join(name);
        File::create(tmp_dir.path().join(name))
            .unwrap()
            .write_all(format!("{:#}", contents).as_bytes())
            .unwrap();
        return path;
    }

    macro_rules! fini_file {
        ( $( $line:literal ),* ) => {
            {
                let mut buf: Vec<&str> = Vec::new();
                $(
                    buf.push($line);
                )*
                buf.join("\n")
            }
        };
    }

    #[test]
    fn validate_returns_ok_if_program_binary_empty() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(&tmp_dir, "test.cmx", json!({}));
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
    }

    #[test]
    fn validate_returns_ok_for_proper_cmx() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "bin/hello_world"
                },
                "runner": "elf"
            }),
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
    }

    #[test]
    fn validate_returns_ok_for_proper_cml() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cml",
            r#"{
                // JSON5, which .cml uses, allows for comments.
                program: {
                    runner: "elf",
                    binary: "bin/hello_world",
                },
            }"#,
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
    }

    #[test]
    fn validate_returns_ok_for_test_binaries() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "test/hello_world"
                },
                "runner": "elf"
            }),
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("test/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
    }

    #[test]
    fn validate_returns_ok_for_disabled_test_binaries() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "test/hello_world"
                },
                "runner": "elf"
            }),
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("test/disabled/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
    }

    #[test]
    fn validate_returns_ok_for_ignored_runners() {
        let tmp_dir = TempDir::new().unwrap();
        let package_manifest = tmp_file(&tmp_dir, "test.fini", "");

        // Normally, an empty package manifest file would yield an error.
        // However, the following runners are ignored during validation.
        for runner in [
            "fuchsia-pkg://fuchsia.com/guest-runner#meta/guest_runner.cmx",
            "fuchsia-pkg://fuchsia.com/appmgr_mock_runner#meta/appmgr_mock_runner.cmx",
            "fuchsia-pkg://fuchsia.com/netemul-runner#meta/netemul-runner.cmx",
            "fuchsia-pkg://fuchsia.com/dart_jit_runner#meta/dart_jit_runner.cmx",
        ]
        .iter()
        {
            let component_manifest = tmp_file(
                &tmp_dir,
                "test.cmx",
                json!({
                    "program": {
                        "binary": "bin/hello_world"
                    },
                    "runner": runner
                }),
            );

            assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
        }
    }

    #[test]
    fn validate_returns_ok_if_runner_not_provided() {
        let tmp_dir = TempDir::new().unwrap();
        let package_manifest = tmp_file(&tmp_dir, "test.fini", fini_file!("bin/hello_world"));

        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "bin/hello_world"
                }
            }),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Ok(()));
    }

    #[test]
    fn validate_returns_err_if_runner_not_provided_and_binary_not_found() {
        let tmp_dir = TempDir::new().unwrap();
        let package_manifest = tmp_file(&tmp_dir, "test.fini", fini_file!("bin/hello_world"));

        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "test/hello_world"
                }
            }),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Err(_));
    }

    #[test]
    fn validate_returns_err_if_package_manifest_is_empty() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "bin/hello_world"
                },
                "runner": "elf"
            }),
        );
        let package_manifest = tmp_file(&tmp_dir, "test.fini", "");

        assert_matches!(validate(&component_manifest, &package_manifest, None), Err(_));
    }

    #[test]
    fn validate_returns_err_if_binary_not_found() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            json!({
                "program": {
                    "binary": "bin/not_a_listed_binary"
                },
                "runner": "elf"
            }),
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Err(_));
    }

    #[test]
    fn validate_returns_err_if_component_manifest_has_unknown_extension() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.txt",
            r#"{
                // JSON5, which .cml uses, allows for comments.
                program: {
                    binary: "bin/hello_world",
                },
            }"#,
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Err(_));
    }

    #[test]
    fn validate_returns_err_if_cml_is_malformed() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cml",
            r#"
            // With no opening '{', the json5 parser should yield an error.
                program: {
                    binary: "bin/hello_world",
                }
            "#,
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Err(_));
    }

    #[test]
    fn validate_returns_err_if_cmx_is_malformed() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cmx",
            // json parser should break when encountering unquoted keys.
            r#"{
                program: {
                    binary: "bin/hello_world"
                }
                runner: "elf",
            }"#,
        );
        let package_manifest = tmp_file(
            &tmp_dir,
            "test.fini",
            fini_file!("bin/hello_world=hello_world", "lib/foo=foo"),
        );

        assert_matches!(validate(&component_manifest, &package_manifest, None), Err(_));
    }

    #[test]
    fn validate_returns_err_if_package_manifest_is_bad_file() {
        let tmp_dir = TempDir::new().unwrap();
        let component_manifest = tmp_file(
            &tmp_dir,
            "test.cml",
            r#"{
                // JSON5, which .cml uses, allows for comments.
                program: {
                    binary: "bin/hello_world",
                },
                use: [{  runner: "elf", }],
            }"#,
        );

        assert_matches!(
            validate(&component_manifest, &PathBuf::from("file/doesnt/exist"), None),
            Err(_)
        );
    }

    #[test]
    fn get_nearest_match_returns_correct_value() {
        assert_eq!(
            get_nearest_match("foo", &vec!["lib/bar".to_string(), "bin/foo".to_string()]),
            "bin/foo"
        );
        assert_eq!(
            get_nearest_match("foo", &vec!["bin/foo".to_string(), "lib/bar".to_string()]),
            "bin/foo"
        );
    }
}

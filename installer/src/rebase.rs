//! Rebase the maintained patch set onto a freshly generated tree without mutating either input.

use crate::diff;
use crate::hash;
use crate::lock::{Entry, Lock};
use crate::manifest::Manifest;
use crate::pipeline::SKIP_DIRS;
use crate::spu_alias::Aliases;
use crate::util::{self, Ctx, Error, Result};
use std::collections::{BTreeSet, HashMap};
use std::fs;
use std::path::Path;

#[derive(Debug)]
pub struct RebaseReport {
    pub lock: Lock,
    pub unchanged: usize,
    pub patched: usize,
    pub created: usize,
    pub removed: usize,
}

pub fn rebase_lock(
    manifest: &Manifest,
    fresh: &Path,
    installer_dir: &Path,
    source: &Lock,
    eboot_sha: &str,
    aliases: &Aliases,
    patch_overrides: &HashMap<String, Vec<u8>>,
) -> Result<RebaseReport> {
    let known: BTreeSet<&str> = source
        .files
        .iter()
        .map(|entry| entry.path.as_str())
        .collect();
    let present = util::select_files(
        fresh,
        &manifest.gen_include,
        &manifest.gen_exclude,
        SKIP_DIRS,
    )?;
    let unexpected: Vec<&str> = present
        .iter()
        .map(String::as_str)
        .filter(|path| {
            let eu = aliases.reverse_path(path);
            !known.contains(eu.as_str()) && !manifest.is_loose(path)
        })
        .collect();
    if !unexpected.is_empty() {
        crate::bail!(
            "unexpected generated file {}",
            unexpected.join("\nunexpected generated file ")
        );
    }

    let mut lock = Lock {
        eboot: Some(eboot_sha.to_string()),
        files: Vec::with_capacity(source.files.len()),
    };
    let (mut unchanged, mut patched, mut created, mut removed) = (0, 0, 0, 0);

    for source_entry in &source.files {
        let us_path = aliases.remap_path(&source_entry.path);
        let fresh_path = util::join_slash(fresh, &us_path);
        let patch_path = patched_path(installer_dir, &us_path);

        if source_entry.fin.is_none() {
            let base = if fresh_path.is_file() {
                Some(hash::sha256_file(&fresh_path).ctx(|| {
                    format!(
                        "{}: hashing fresh file {}",
                        source_entry.path,
                        fresh_path.display()
                    )
                })?)
            } else {
                None
            };
            lock.files.push(Entry {
                path: us_path.clone(),
                base,
                fin: None,
            });
            removed += 1;
            continue;
        }

        if source_entry.base == source_entry.fin && !patch_path.exists() {
            let bytes = fs::read(&fresh_path).ctx(|| {
                format!(
                    "{}: reading fresh file {}",
                    source_entry.path,
                    fresh_path.display()
                )
            })?;
            let digest = hash::sha256_hex(&bytes);
            lock.files.push(Entry {
                path: us_path.clone(),
                base: Some(digest.clone()),
                fin: Some(digest),
            });
            unchanged += 1;
            continue;
        }

        if !patch_path.is_file() {
            crate::bail!(
                "{}: missing patch {}",
                source_entry.path,
                patch_path.display()
            );
        }
        let patch = if let Some(p) = patch_overrides.get(&us_path) {
            p.clone()
        } else {
            fs::read(&patch_path).ctx(|| {
                format!(
                    "{}: reading patch {}",
                    source_entry.path,
                    patch_path.display()
                )
            })?
        };
        let (base, old) = if source_entry.base.is_none() {
            (None, Vec::new())
        } else {
            let old = fs::read(&fresh_path).ctx(|| {
                format!(
                    "{}: reading fresh file {}",
                    source_entry.path,
                    fresh_path.display()
                )
            })?;
            let digest = hash::sha256_hex(&old);
            (Some(digest), old)
        };
        let new = diff::apply_patch(&old, &patch)
            .map_err(|error| Error(format!("{}: {error}", source_entry.path)))?;
        let fin = Some(hash::sha256_hex(&new));
        lock.files.push(Entry {
            path: us_path.clone(),
            base,
            fin,
        });
        if source_entry.base.is_none() {
            created += 1;
        } else {
            patched += 1;
        }
    }

    Ok(RebaseReport {
        lock,
        unchanged,
        patched,
        created,
        removed,
    })
}

fn patched_path(installer_dir: &Path, relative: &str) -> std::path::PathBuf {
    util::join_slash(&installer_dir.join("patches"), &format!("{relative}.patch"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::diff::make_patch;
    use crate::hash::sha256_hex;
    use crate::lock::{Entry, Lock};
    use crate::manifest::Manifest;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{SystemTime, UNIX_EPOCH};

    static NEXT_FIXTURE: AtomicU64 = AtomicU64::new(0);

    #[derive(Clone, Copy)]
    enum PatchSpec<'a> {
        Replace(&'a str, &'a str),
        Create(&'a str),
        Delete(&'a str),
    }

    impl PatchSpec<'_> {
        fn old_bytes(self) -> Option<Vec<u8>> {
            match self {
                Self::Replace(old, _) | Self::Delete(old) => Some(old.as_bytes().to_vec()),
                Self::Create(_) => None,
            }
        }

        fn new_bytes(self) -> Option<Vec<u8>> {
            match self {
                Self::Replace(_, new) | Self::Create(new) => Some(new.as_bytes().to_vec()),
                Self::Delete(_) => None,
            }
        }
    }

    struct Fixture {
        root: PathBuf,
        fresh: PathBuf,
        installer: PathBuf,
    }

    impl Fixture {
        fn new() -> Self {
            let nonce = NEXT_FIXTURE.fetch_add(1, Ordering::Relaxed);
            let nanos = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let root = std::env::temp_dir().join(format!(
                "infamous-rebase-test-{}-{nanos}-{nonce}",
                std::process::id()
            ));
            let fresh = root.join("fresh");
            let installer = root.join("installer");
            fs::create_dir_all(&fresh).unwrap();
            fs::create_dir_all(&installer).unwrap();
            Self {
                root,
                fresh,
                installer,
            }
        }

        fn write(&self, root: &Path, rel: &str, bytes: &[u8]) {
            let path = rel
                .split('/')
                .filter(|part| !part.is_empty())
                .fold(root.to_path_buf(), |path, part| path.join(part));
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.root);
        }
    }

    fn hash(text: &str) -> String {
        sha256_hex(text.as_bytes())
    }

    fn manifest() -> Manifest {
        Manifest::parse(
            r#"
[game]
title = "inFamous"
title_ids = ["BCUS98119"]
eboot_sha256 = []
link_name = "Infamous (USA) (En,Fr,Es) (v02.00)"

[layout]
port = "infamous-recomp"
toolkit = "ps3recomp"

[python]
min_version = "3.10"
requirements = []
packages = []

[generated]
include = ["generated/"]
exclude = []
loose = []
full_patch_ok = []

[build]
configure = []
compile = []
exe = "inFamous.exe"

[launch]
name = "inFamous"
"#,
        )
        .unwrap()
    }

    fn write_source_lock(fixture: &Fixture, path: &str, base: Option<&str>, fin: Option<&str>) {
        let lock = Lock {
            eboot: Some(hash("eu-eboot\n")),
            files: vec![Entry {
                path: path.to_string(),
                base: base.map(hash),
                fin: fin.map(hash),
            }],
        };
        lock.save(&fixture.installer.join("generated.lock"))
            .unwrap();
    }

    fn write_patch(fixture: &Fixture, path: &str, spec: PatchSpec<'_>) {
        let old = spec.old_bytes().unwrap_or_default();
        let new = spec.new_bytes().unwrap_or_default();
        let (patch, _) = make_patch(&old, &new, path, 3);
        fixture.write(
            &fixture.installer.join("patches"),
            &format!("{path}.patch"),
            &patch,
        );
    }

    fn run_case(
        path: &str,
        kept_text: &str,
        patch: Option<PatchSpec<'_>>,
        fresh_text: &str,
    ) -> RebaseReport {
        let fixture = Fixture::new();
        let (base, fin) = match patch {
            Some(PatchSpec::Replace(old, new)) => (Some(old), Some(new)),
            Some(PatchSpec::Create(new)) => (None, Some(new)),
            Some(PatchSpec::Delete(old)) => (Some(old), None),
            None => (Some(kept_text), Some(kept_text)),
        };
        write_source_lock(&fixture, path, base, fin);
        if !fresh_text.is_empty() {
            fixture.write(&fixture.fresh, path, fresh_text.as_bytes());
        }
        if let Some(spec) = patch {
            write_patch(&fixture, path, spec);
        }
        let source = Lock::load(&fixture.installer.join("generated.lock"))
            .unwrap()
            .unwrap();
        rebase_lock(
            &manifest(),
            &fixture.fresh,
            &fixture.installer,
            &source,
            &hash("us-eboot\n"),
            &Aliases::default(),
            &HashMap::new(),
        )
        .unwrap_or_else(|error| panic!("{error}"))
    }

    fn run_bad_case(path: &str, fresh_text: &str, patch: (&str, &str)) -> String {
        let fixture = Fixture::new();
        write_source_lock(&fixture, path, Some(patch.0), Some(patch.1));
        fixture.write(&fixture.fresh, path, fresh_text.as_bytes());
        write_patch(&fixture, path, PatchSpec::Replace(patch.0, patch.1));
        let source = Lock::load(&fixture.installer.join("generated.lock"))
            .unwrap()
            .unwrap();
        match rebase_lock(
            &manifest(),
            &fixture.fresh,
            &fixture.installer,
            &source,
            &hash("us-eboot\n"),
            &Aliases::default(),
            &HashMap::new(),
        ) {
            Ok(_) => panic!("rebase unexpectedly returned a lock"),
            Err(error) => error.to_string(),
        }
    }

    fn run_unexpected_file_case(path: &str) -> String {
        let fixture = Fixture::new();
        let expected = "generated/expected.txt";
        let expected_text = "expected\n";
        write_source_lock(&fixture, expected, Some(expected_text), Some(expected_text));
        fixture.write(&fixture.fresh, expected, expected_text.as_bytes());
        fixture.write(&fixture.fresh, path, b"unexpected\n");
        let source = Lock::load(&fixture.installer.join("generated.lock"))
            .unwrap()
            .unwrap();
        match rebase_lock(
            &manifest(),
            &fixture.fresh,
            &fixture.installer,
            &source,
            &hash("us-eboot\n"),
            &Aliases::default(),
            &HashMap::new(),
        ) {
            Ok(_) => panic!("rebase unexpectedly returned a lock"),
            Err(error) => error.to_string(),
        }
    }

    #[test]
    fn unchanged_entry_uses_us_base_hash() {
        let r = run_case("generated/a.txt", "eu-a\n", None, "us-a\n");
        assert_eq!(r.unchanged, 1);
        assert_eq!(r.lock.files[0].base, r.lock.files[0].fin);
    }

    #[test]
    fn applies_existing_patch_to_new_us_base() {
        let eu_base = "\
line0\n\
line1\n\
line2\n\
line3=eu: x\n\
line4\n\
line5\n\
line6\n\
line7\n\
line8=eu-tail\n";
        let eu_patched = "\
line0\n\
line1\n\
line2\n\
line3=eu: y\n\
line4\n\
line5\n\
line6\n\
line7\n\
line8=eu-tail\n";
        let us_base = "\
line0\n\
line1\n\
line2\n\
line3=eu: x\n\
line4\n\
line5\n\
line6\n\
line7\n\
line8=us-tail\n";
        let us_patched = "\
line0\n\
line1\n\
line2\n\
line3=eu: y\n\
line4\n\
line5\n\
line6\n\
line7\n\
line8=us-tail\n";
        let r = run_case(
            "generated/b.txt",
            eu_patched,
            Some(PatchSpec::Replace(eu_base, eu_patched)),
            us_base,
        );
        assert_eq!(r.patched, 1);
        assert_eq!(
            r.lock.files[0].base.as_deref(),
            Some(hash(us_base).as_str())
        );
        assert_eq!(
            r.lock.files[0].fin.as_deref(),
            Some(hash(us_patched).as_str())
        );
    }

    #[test]
    fn preserves_a_patch_created_file() {
        let r = run_case(
            "generated/c.txt",
            "",
            Some(PatchSpec::Create("created\n")),
            "",
        );
        assert_eq!(r.created, 1);
        assert_eq!(r.lock.files[0].base, None);
        assert_eq!(
            r.lock.files[0].fin.as_deref(),
            Some(hash("created\n").as_str())
        );
    }

    #[test]
    fn preserves_a_deleted_generated_file() {
        let r = run_case(
            "generated/d.txt",
            "us-d\n",
            Some(PatchSpec::Delete("eu-d\n")),
            "us-d\n",
        );
        assert_eq!(r.removed, 1);
        assert_eq!(r.lock.files[0].fin, None);
    }

    #[test]
    fn context_mismatch_names_the_file_and_does_not_return_a_lock() {
        let e = run_bad_case(
            "generated/e.txt",
            "\
line0\n\
line1=us-context\n\
line2\n\
line3=eu-old\n\
line4\n",
            (
                "\
line0\n\
line1=eu-context\n\
line2\n\
line3=eu-old\n\
line4\n",
                "\
line0\n\
line1=eu-context\n\
line2\n\
line3=eu-new\n\
line4\n",
            ),
        );
        assert!(e.contains("generated/e.txt"));
        assert!(e.contains("context mismatch"));
    }

    #[test]
    fn unexpected_generated_file_aborts_the_rebase() {
        let e = run_unexpected_file_case("generated/extra.cpp");
        assert!(e.contains("unexpected generated file generated/extra.cpp"));
    }
}

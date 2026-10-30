//! US-vs-EU SPU group aliasing for `rebase-patches`.
//!
//! The eight embedded SPU programs are byte-identical between the EU and US EBOOTs but sit at
//! different file offsets. The pipeline names every image and the lifted sources derived from it
//! by that offset (`spu_0000_at_005F6500` in the EU tree, `spu_0000_at_005F6000` in the US tree).
//! Rebasing the EU patch set onto a US dump therefore needs a deterministic, strict rename of
//! eight group names. This module derives that mapping and applies it to the committed patches.

use crate::hash;
use crate::lock::Lock;
use crate::util::{self, Ctx, Result};
use std::collections::BTreeMap;
use std::path::Path;

/// A single EU -> US group rename, e.g. `spu_0000_at_005F6500` -> `spu_0000_at_005F6000`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Group {
    pub index: u32,
    pub eu: String,
    pub us: String,
}

/// The eight EU -> US group renames, or empty when the patch set already matches the dump.
#[derive(Debug, Default, Clone)]
pub struct Aliases {
    groups: Vec<Group>,
}

impl Aliases {
    pub fn groups(&self) -> &[Group] {
        &self.groups
    }

    pub fn is_empty(&self) -> bool {
        self.groups.is_empty()
    }

    /// One log line: `spu_0000_at_005F6500->spu_0000_at_005F6000, ...`.
    pub fn describe(&self) -> String {
        self.groups
            .iter()
            .map(|g| format!("{}->{}", g.eu, g.us))
            .collect::<Vec<_>>()
            .join(", ")
    }

    /// Replace every EU group name with its US group name (exact bytes, nothing else).
    pub fn remap_bytes(&self, data: &[u8]) -> Vec<u8> {
        let mut out = data.to_vec();
        for g in &self.groups {
            if g.eu != g.us {
                out = replace_all(&out, g.eu.as_bytes(), g.us.as_bytes());
            }
        }
        out
    }

    /// Rewrite a path string, replacing EU group names with US group names.
    pub fn remap_path(&self, path: &str) -> String {
        let mut out = path.to_string();
        for g in &self.groups {
            if g.eu != g.us {
                out = out.replace(&g.eu, &g.us);
            }
        }
        out
    }

    /// The inverse: rewrite a US path back to the EU source path it came from.
    pub fn reverse_path(&self, path: &str) -> String {
        let mut out = path.to_string();
        for g in &self.groups {
            if g.eu != g.us {
                out = out.replace(&g.us, &g.eu);
            }
        }
        out
    }
}

fn replace_all(haystack: &[u8], needle: &[u8], replacement: &[u8]) -> Vec<u8> {
    if needle.is_empty() {
        return haystack.to_vec();
    }
    let mut out = Vec::with_capacity(haystack.len());
    let mut i = 0;
    while i < haystack.len() {
        if haystack[i..].starts_with(needle) {
            out.extend_from_slice(replacement);
            i += needle.len();
        } else {
            out.push(haystack[i]);
            i += 1;
        }
    }
    out
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && haystack.windows(needle.len()).any(|w| w == needle)
}

/// Parse `spu_0000_at_005F6500` into `(0, "spu_0000_at_005F6500")`.
fn parse_group(stem: &str) -> Option<(u32, String)> {
    let rest = stem.strip_prefix("spu_")?;
    let (idx, off) = rest.split_once("_at_")?;
    let index: u32 = idx.parse().ok()?;
    if off.len() != 8 || !off.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    Some((index, stem.to_string()))
}

/// Extract `(index, group)` from an image file name like `spu_0000_at_005F6500.elf`.
fn image_file(name: &str) -> Option<(u32, String)> {
    parse_group(name.strip_suffix(".elf")?)
}

/// Extract `(index, group)` from a lock entry path like
/// `infamous-recomp/spu-images/spu_0000_at_005F6500.elf`.
fn image_entry(path: &str) -> Option<(u32, String)> {
    image_file(path.rsplit('/').next()?)
}

/// Validate the mapping is collision-free: no duplicate names, no name that is a substring of
/// another, and no US name that equals another group's EU name. This keeps byte remapping
/// unambiguous and order-independent.
pub fn validate(groups: &[Group]) -> Result<()> {
    for (i, a) in groups.iter().enumerate() {
        for (j, b) in groups.iter().enumerate() {
            if i == j {
                continue;
            }
            if a.eu == b.eu {
                crate::bail!("duplicate EU group name {}", a.eu);
            }
            if a.us == b.us {
                crate::bail!("duplicate US group name {}", a.us);
            }
            if a.eu.contains(&b.eu) || b.eu.contains(&a.eu) {
                crate::bail!("overlapping EU group names {} / {}", a.eu, b.eu);
            }
            if a.us.contains(&b.us) || b.us.contains(&a.us) {
                crate::bail!("overlapping US group names {} / {}", a.us, b.us);
            }
            if a.eu == b.us || a.us == b.eu {
                crate::bail!("EU group name {} collides with a US group name", a.eu);
            }
        }
    }
    Ok(())
}

/// Derive the EU -> US group mapping from the EU source lock and the freshly generated US tree.
///
/// Every embedded image is matched by its `spu_000N` index AND the exact sha256 of its bytes.
/// A missing image, a duplicated index, or a byte mismatch is a hard error.
pub fn derive(source: &Lock, fresh: &Path) -> Result<Aliases> {
    let mut eu: BTreeMap<u32, (String, String)> = BTreeMap::new(); // index -> (name, base)
    for e in &source.files {
        if let Some((index, name)) = image_entry(&e.path) {
            let base = e
                .base
                .clone()
                .ok_or_else(|| util::Error(format!("{}: image entry has no base hash", e.path)))?;
            if eu.insert(index, (name, base)).is_some() {
                crate::bail!("the source lock has two SPU images with index {index}");
            }
        }
    }

    let images_dir = fresh.join("infamous-recomp").join("spu-images");
    let mut us: BTreeMap<u32, (String, String)> = BTreeMap::new(); // index -> (name, sha)
    if images_dir.is_dir() {
        for ent in
            std::fs::read_dir(&images_dir).ctx(|| format!("reading {}", images_dir.display()))?
        {
            let ent = ent?;
            let fname = ent.file_name().to_string_lossy().into_owned();
            if let Some((index, name)) = image_file(&fname) {
                let sha = hash::sha256_file(&ent.path())?;
                if us.insert(index, (name, sha)).is_some() {
                    crate::bail!("the US tree has two SPU images with index {index}");
                }
            }
        }
    }

    if eu.is_empty() {
        return Ok(Aliases::default());
    }
    if eu.len() != us.len() {
        crate::bail!(
            "SPU image count mismatch: {} in the source lock, {} in the US tree",
            eu.len(),
            us.len()
        );
    }

    let mut groups = Vec::with_capacity(eu.len());
    for (index, (eu_name, eu_hash)) in &eu {
        let Some((us_name, us_hash)) = us.get(index) else {
            crate::bail!("no US SPU image for index {index} ({eu_name})");
        };
        if eu_hash != us_hash {
            crate::bail!(
                "SPU image {eu_name} does not match the US image {us_name}: {eu_hash} != {us_hash} \
                 (non-identical content)"
            );
        }
        groups.push(Group {
            index: *index,
            eu: eu_name.clone(),
            us: us_name.clone(),
        });
    }
    validate(&groups)?;
    if groups.iter().all(|g| g.eu == g.us) {
        return Ok(Aliases::default());
    }
    Ok(Aliases { groups })
}

/// Persist the rename in the committed patch set: move each `spu_000N_at_<eu>` directory to its US
/// name, and remap the bytes of every patch file that still carries an EU group name. Files without
/// a group name are left untouched. The operation is idempotent.
pub fn transform_patches(installer_dir: &Path, aliases: &Aliases) -> Result<()> {
    if aliases.is_empty() {
        return Ok(());
    }
    let pdir = installer_dir.join("patches");
    let spu = pdir.join("infamous-recomp").join("spu");

    for g in &aliases.groups {
        if g.eu == g.us {
            continue;
        }
        let from = spu.join(&g.eu);
        let to = spu.join(&g.us);
        if from.is_dir() {
            if to.exists() {
                crate::bail!(
                    "both {} and {} exist -- ambiguous patch directories",
                    from.display(),
                    to.display()
                );
            }
            std::fs::rename(&from, &to)
                .ctx(|| format!("renaming {} -> {}", from.display(), to.display()))?;
        } else if !to.is_dir() {
            crate::bail!("missing patch directory {}", from.display());
        }
        // else: already renamed by an earlier run.
    }

    let files = util::walk_files(&pdir, &[], &|_| true)?;
    for rel in &files {
        let path = util::join_slash(&pdir, rel);
        let data = std::fs::read(&path).ctx(|| format!("reading {}", path.display()))?;
        let touched = aliases
            .groups
            .iter()
            .any(|g| g.eu != g.us && contains(&data, g.eu.as_bytes()));
        if touched {
            let remapped = aliases.remap_bytes(&data);
            util::write_file(&path, &remapped)?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::diff;
    use crate::lock::Entry;

    fn h(s: &str) -> String {
        crate::hash::sha256_hex(s.as_bytes())
    }

    fn image_path(name: &str) -> String {
        format!("infamous-recomp/spu-images/{name}.elf")
    }

    fn lock_with(images: &[(&str, &str)]) -> Lock {
        Lock {
            eboot: Some("x".into()),
            files: images
                .iter()
                .map(|(name, bytes)| Entry {
                    path: image_path(name),
                    base: Some(h(bytes)),
                    fin: Some(h(bytes)),
                })
                .collect(),
        }
    }

    fn write_fresh(root: &Path, images: &[(&str, &str)]) {
        let dir = root.join("infamous-recomp").join("spu-images");
        std::fs::create_dir_all(&dir).unwrap();
        for (name, bytes) in images {
            std::fs::write(dir.join(format!("{name}.elf")), bytes).unwrap();
        }
    }

    fn tempdir(tag: &str) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("spu-alias-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p);
        std::fs::create_dir_all(&p).unwrap();
        p
    }

    const NAMES: [(&str, &str); 8] = [
        ("spu_0000_at_005F6500", "spu_0000_at_005F6000"),
        ("spu_0001_at_00674380", "spu_0001_at_00673E80"),
        ("spu_0002_at_0068FE00", "spu_0002_at_0068F900"),
        ("spu_0003_at_006A3800", "spu_0003_at_006A3300"),
        ("spu_0004_at_006BF480", "spu_0004_at_006BEF80"),
        ("spu_0005_at_006D7700", "spu_0005_at_006D7200"),
        ("spu_0006_at_006F1E00", "spu_0006_at_006F1900"),
        ("spu_0007_at_00700600", "spu_0007_at_00700100"),
    ];

    #[test]
    fn derives_unique_mapping_by_index_and_hash() {
        let root = tempdir("derive");
        // Image bytes are identical between releases; only the offset-derived names differ.
        let contents: Vec<String> = (0..8).map(|i| format!("image {i}\n")).collect();
        let eu: Vec<(&str, &str)> = NAMES
            .iter()
            .enumerate()
            .map(|(i, (eu, _))| (*eu, contents[i].as_str()))
            .collect();
        let us: Vec<(&str, &str)> = NAMES
            .iter()
            .enumerate()
            .map(|(i, (_, us))| (*us, contents[i].as_str()))
            .collect();
        let lock = lock_with(&eu);
        write_fresh(&root, &us);
        let aliases = derive(&lock, &root).unwrap();
        assert_eq!(aliases.groups.len(), 8);
        for (i, g) in aliases.groups.iter().enumerate() {
            assert_eq!(g.eu, NAMES[i].0);
            assert_eq!(g.us, NAMES[i].1);
        }
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn rejects_missing_us_image() {
        let root = tempdir("missing");
        let eu: Vec<(&str, &str)> = NAMES.iter().map(|(eu, _)| (*eu, *eu)).collect();
        let mut us: Vec<(&str, &str)> = NAMES.iter().map(|(_, us)| (*us, *us)).collect();
        us.pop();
        let lock = lock_with(&eu);
        write_fresh(&root, &us);
        let err = derive(&lock, &root).unwrap_err();
        assert!(err.to_string().contains("count mismatch"));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn rejects_non_identical_image_bytes() {
        let root = tempdir("mismatch");
        let eu: Vec<(&str, &str)> = NAMES.iter().map(|(eu, _)| (*eu, *eu)).collect();
        let mut us: Vec<(&str, &str)> = NAMES.iter().map(|(_, us)| (*us, *us)).collect();
        us[0].1 = "different bytes";
        let lock = lock_with(&eu);
        write_fresh(&root, &us);
        let err = derive(&lock, &root).unwrap_err();
        assert!(err.to_string().contains("non-identical content"));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn rejects_ambiguous_duplicate_index() {
        let root = tempdir("ambiguous");
        let eu: Vec<(&str, &str)> = NAMES.iter().map(|(eu, _)| (*eu, *eu)).collect();
        let mut us: Vec<(&str, &str)> = NAMES.iter().map(|(_, us)| (*us, *us)).collect();
        us.push(("spu_0000_at_00AAAAAA", "duplicate index"));
        let lock = lock_with(&eu);
        write_fresh(&root, &us);
        let err = derive(&lock, &root).unwrap_err();
        assert!(err.to_string().contains("two SPU images with index"));
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn validate_rejects_collisions() {
        let overlapping = vec![
            Group {
                index: 0,
                eu: "spu_0000_at_005F6500".into(),
                us: "spu_0000_at_005F6000".into(),
            },
            Group {
                index: 1,
                eu: "spu_0000_at_005F6500_extra".into(),
                us: "spu_0001_at_00673E80".into(),
            },
        ];
        assert!(validate(&overlapping).is_err());

        let crossed = vec![
            Group {
                index: 0,
                eu: "spu_0000_at_005F6500".into(),
                us: "spu_0001_at_00674380".into(),
            },
            Group {
                index: 1,
                eu: "spu_0001_at_00674380".into(),
                us: "spu_0000_at_005F6500".into(),
            },
        ];
        assert!(validate(&crossed).is_err());
    }

    #[test]
    fn remap_makes_patch_apply_on_renamed_base() {
        let aliases = Aliases {
            groups: vec![Group {
                index: 0,
                eu: "spu_0000_at_005F6500".into(),
                us: "spu_0000_at_005F6000".into(),
            }],
        };
        let eu_base = "void spu_0000_at_005F6500_spu_func_0() { old; }\n";
        let eu_fin = "void spu_0000_at_005F6500_spu_func_0() { new; }\n";
        let us_base = "void spu_0000_at_005F6000_spu_func_0() { old; }\n";
        let us_fin = "void spu_0000_at_005F6000_spu_func_0() { new; }\n";
        let (patch, _) = diff::make_patch(
            eu_base.as_bytes(),
            eu_fin.as_bytes(),
            "spu_0000_at_005F6500/x.c",
            3,
        );
        // The un-remapped patch must not apply to the US base.
        assert!(diff::apply_patch(us_base.as_bytes(), &patch).is_err());
        let remapped = aliases.remap_bytes(&patch);
        let out = diff::apply_patch(us_base.as_bytes(), &remapped).unwrap();
        assert_eq!(out, us_fin.as_bytes());
    }
}

//! Rebase the EU PPU hand-edits onto the US lift.
//!
//! The US and EU PPU code share the same lifted-code shapes but their function addresses differ,
//! so the EU patches cannot be applied line-for-line. The edits split into three classes:
//!   * global mechanical transforms -- `memcpy(vm_base + (uint32_t)ea, &ctx->vr[N], 16)` ->
//!     `ppu_stvx16((uint32_t)ea, &ctx->vr[N])`, and `memset(vm_base + (uint32_t)ea, 0, 128)` ->
//!     `ppu_dcbz128((uint32_t)ea)` -- applied by exact whole-line replacement;
//!   * encoding-only no-ops (em/en dash <-> U+FFFD), which are skipped;
//!   * residual context-specific hunks, applied only where their whole old side has a unique exact
//!     match in the US file, otherwise a hard stop.
//!
//! The header patch is a pure-insertion patch whose hunk lines carry CRLF; its line endings are
//! normalized to the US file's LF form and it is applied through the strict diff engine.

use crate::diff;
use crate::util::{Error, Result};
use std::collections::HashMap;

const STORE_HEAD: &[u8] = b"memcpy(vm_base + (uint32_t)ea, &ctx->vr[";
const STORE_TAIL: &[u8] = b"], 16)";
const STORE_REPL_HEAD: &[u8] = b"ppu_stvx16((uint32_t)ea, &ctx->vr[";
const DCBZ_OLD: &[u8] = b"memset(vm_base + (uint32_t)ea, 0, 128)";
const DCBZ_NEW: &[u8] = b"ppu_dcbz128((uint32_t)ea)";
const U_FFFD: &[u8] = b"\xef\xbf\xbd";
const EM_DASH: &[u8] = b"\xe2\x80\x94";
const EN_DASH: &[u8] = b"\xe2\x80\x93";
const CP1252_DASH: &[u8] = b"\x97";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Mech {
    Stvx16,
    Dcbz128,
    Encoding,
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

fn find_sub(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return None;
    }
    haystack.windows(needle.len()).position(|w| w == needle)
}

/// Replace exactly one `memcpy(vm_base + (uint32_t)ea, &ctx->vr[N], 16)` store with the
/// `ppu_stvx16` call. Returns None when the line has no unambiguous single store site.
fn transform_store(line: &[u8]) -> Option<Vec<u8>> {
    let i = find_sub(line, STORE_HEAD)?;
    let j = find_sub(&line[i..], STORE_TAIL)?;
    let reg = &line[i + STORE_HEAD.len()..i + j];
    if reg.is_empty() || !reg.iter().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let mut out = Vec::with_capacity(line.len());
    out.extend_from_slice(&line[..i]);
    out.extend_from_slice(STORE_REPL_HEAD);
    out.extend_from_slice(reg);
    out.extend_from_slice(b"])");
    out.extend_from_slice(&line[i + j + STORE_TAIL.len()..]);
    Some(out)
}

fn transform_dcbz(line: &[u8]) -> Option<Vec<u8>> {
    let i = find_sub(line, DCBZ_OLD)?;
    let mut out = Vec::with_capacity(line.len());
    out.extend_from_slice(&line[..i]);
    out.extend_from_slice(DCBZ_NEW);
    out.extend_from_slice(&line[i + DCBZ_OLD.len()..]);
    Some(out)
}

fn canonical(bytes: &[u8]) -> Vec<u8> {
    // Fold the dash family (UTF-8 em/en dash, U+FFFD, Windows-1252 0x97) onto a sentinel that
    // never occurs in text, so unrelated '~' content is not treated as a dash.
    let v = replace_all(bytes, U_FFFD, b"\x00");
    let v = replace_all(&v, EM_DASH, b"\x00");
    let v = replace_all(&v, EN_DASH, b"\x00");
    replace_all(&v, CP1252_DASH, b"\x00")
}

fn encoding_noop(a: &[u8], b: &[u8]) -> bool {
    a != b && canonical(a) == canonical(b)
}

/// Classify an old->new line pair. Only unambiguous, semantically exact mechanical transforms are
/// accepted; everything else is left for residual context matching.
fn mechanical(old: &[u8], new: &[u8]) -> Option<Mech> {
    if encoding_noop(old, new) {
        return Some(Mech::Encoding);
    }
    if transform_store(old).as_deref() == Some(new) {
        return Some(Mech::Stvx16);
    }
    if transform_dcbz(old).as_deref() == Some(new) {
        return Some(Mech::Dcbz128);
    }
    None
}

struct HunkOwned {
    old: Vec<Vec<u8>>,
    new: Vec<Vec<u8>>,
    dels: Vec<Vec<u8>>,
    ins: Vec<Vec<u8>>,
}

fn parse_hunks(patch: &[u8]) -> Result<Vec<HunkOwned>> {
    let normalized = replace_all(patch, b"\r\n", b"\n");
    let raw = diff::parse_patch(&normalized).map_err(|e| Error(e))?;
    let mut out = Vec::new();
    for h in raw {
        let mut owned = HunkOwned {
            old: Vec::new(),
            new: Vec::new(),
            dels: Vec::new(),
            ins: Vec::new(),
        };
        for (tag, body) in &h.lines {
            match *tag {
                b' ' => {
                    owned.old.push(body.to_vec());
                    owned.new.push(body.to_vec());
                }
                b'-' => {
                    owned.old.push(body.to_vec());
                    owned.dels.push(body.to_vec());
                }
                b'+' => {
                    owned.new.push(body.to_vec());
                    owned.ins.push(body.to_vec());
                }
                _ => {}
            }
        }
        out.push(owned);
    }
    Ok(out)
}

fn lines_owned(data: &[u8]) -> Vec<Vec<u8>> {
    diff::split_lines(data).iter().map(|l| l.to_vec()).collect()
}

fn count_matches(lines: &[Vec<u8>], seq: &[Vec<u8>]) -> usize {
    if seq.is_empty() {
        return 0;
    }
    lines.windows(seq.len()).filter(|w| *w == seq).count()
}

fn find_match(lines: &[Vec<u8>], seq: &[Vec<u8>]) -> Option<usize> {
    if seq.is_empty() {
        return None;
    }
    lines.windows(seq.len()).position(|w| w == seq)
}

/// Insert an old -> new mechanical mapping, rejecting a conflicting mapping (the same old line
/// already mapped to a different new line).
fn insert_mapping(
    map: &mut HashMap<Vec<u8>, Vec<u8>>,
    old: &[u8],
    new: Vec<u8>,
    path: &str,
) -> Result<()> {
    if let Some(prev) = map.get(old) {
        if prev != &new {
            crate::bail!(
                "{}: conflicting mapping for line {:?}",
                path,
                String::from_utf8_lossy(old)
            );
        }
    }
    map.insert(old.to_vec(), new);
    Ok(())
}

/// Reconcile a US PPU `.cpp` base against the EU patch: apply residual context-specific hunks at
/// their unique exact match, then apply the global mechanical line replacements.
pub fn reconcile_cpp(path: &str, base: &[u8], patch: &[u8]) -> Result<Vec<u8>> {
    let hunks = parse_hunks(patch)?;
    let mut map: HashMap<Vec<u8>, Vec<u8>> = HashMap::new();
    let mut residual: Vec<&HunkOwned> = Vec::new();

    for h in &hunks {
        let mut fully = h.dels.len() == h.ins.len();
        if fully {
            for (old, new) in h.dels.iter().zip(h.ins.iter()) {
                match mechanical(old, new) {
                    Some(Mech::Stvx16) | Some(Mech::Dcbz128) => {
                        insert_mapping(&mut map, old, new.clone(), path)?;
                    }
                    Some(Mech::Encoding) => {}
                    None => {
                        fully = false;
                        break;
                    }
                }
            }
        }
        if !fully {
            residual.push(h);
        }
    }

    let mut lines = lines_owned(base);

    // Mechanical-map coverage: every distinct global mapping must apply to at least one US line,
    // otherwise a mechanical transform is silently inapplicable (a genuine divergence or a bug).
    {
        let present: std::collections::HashSet<&[u8]> =
            lines.iter().map(|l| l.as_slice()).collect();
        for key in map.keys() {
            if !present.contains(key.as_slice()) {
                crate::bail!(
                    "{}: mechanical mapping not present in the US file: {:?}",
                    path,
                    String::from_utf8_lossy(key)
                );
            }
        }
    }
    for (idx, h) in residual.iter().enumerate() {
        let count = count_matches(&lines, &h.old);
        if count != 1 {
            let first = h.old.first().cloned().unwrap_or_default();
            crate::bail!(
                "{}: residual hunk {} (old {} lines / new {} lines) has {} exact matches; \
                 expected exactly 1. First old-side line: {:?}",
                path,
                idx + 1,
                h.old.len(),
                h.new.len(),
                count,
                String::from_utf8_lossy(&first)
            );
        }
        let pos = find_match(&lines, &h.old).expect("count was 1");
        let mut out = Vec::with_capacity(lines.len() - h.old.len() + h.new.len());
        out.extend_from_slice(&lines[..pos]);
        out.extend_from_slice(&h.new);
        out.extend_from_slice(&lines[pos + h.old.len()..]);
        lines = out;
    }

    for line in &mut lines {
        if let Some(new) = map.get(line) {
            *line = new.clone();
        }
    }
    Ok(lines.concat())
}

/// Apply the EU header patch (pure insertions) after normalizing its CRLF hunk lines to LF.
pub fn reconcile_header(base: &[u8], patch: &[u8]) -> Result<Vec<u8>> {
    let normalized = replace_all(patch, b"\r\n", b"\n");
    diff::apply_patch(base, &normalized).map_err(|e| Error(e))
}

/// Reconcile one PPU file. `is_header` selects the CRLF-normalized strict-apply path for
/// `ppu_recomp.h`; every `.cpp` file goes through the mechanical/residual reconciliation.
pub fn reconcile(path: &str, base: &[u8], patch: &[u8], is_header: bool) -> Result<Vec<u8>> {
    if is_header {
        reconcile_header(base, patch)
    } else {
        reconcile_cpp(path, base, patch)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn store(reg: &str) -> String {
        format!(
            "{{ uint64_t ea = ((ctx->gpr[11] + ctx->gpr[0])) & ~0xFULL; memcpy(vm_base + (uint32_t)ea, &ctx->vr[{reg}], 16); }}\n"
        )
    }
    fn stvx16(reg: &str) -> String {
        format!(
            "{{ uint64_t ea = ((ctx->gpr[11] + ctx->gpr[0])) & ~0xFULL; ppu_stvx16((uint32_t)ea, &ctx->vr[{reg}]); }}\n"
        )
    }

    #[test]
    fn mechanical_store_is_exact() {
        assert_eq!(
            mechanical(store("0").as_bytes(), stvx16("0").as_bytes()),
            Some(Mech::Stvx16)
        );
        assert_eq!(
            mechanical(store("13").as_bytes(), stvx16("13").as_bytes()),
            Some(Mech::Stvx16)
        );
    }

    #[test]
    fn ambiguous_multi_store_line_is_not_mapped() {
        let two = "{\
 memcpy(vm_base + (uint32_t)ea, &ctx->vr[0], 16); \
 memcpy(vm_base + (uint32_t)ea, &ctx->vr[1], 16); }\n";
        let two_new = "{\
 ppu_stvx16((uint32_t)ea, &ctx->vr[0]); \
 ppu_stvx16((uint32_t)ea, &ctx->vr[1]); }\n";
        assert_eq!(mechanical(two.as_bytes(), two_new.as_bytes()), None);
    }

    #[test]
    fn encoding_noop_is_recognized() {
        let old = "/* x \u{2014} y */\n";
        let new = "/* x \u{fffd} y */\n";
        assert_eq!(
            mechanical(old.as_bytes(), new.as_bytes()),
            Some(Mech::Encoding)
        );
    }

    #[test]
    fn cp1252_dash_is_a_comment_only_noop() {
        let old = "/* trap \u{2014} ignored */\n";
        let new = b"/* trap \x97 ignored */\n";
        assert_eq!(mechanical(old.as_bytes(), new), Some(Mech::Encoding));
        // An unrelated literal '~' must NOT be treated as a dash.
        assert_eq!(mechanical(b"a ~ b\n", b"a \x97 b\n"), None);
    }

    #[test]
    fn mechanical_mapping_not_in_us_base_is_rejected() {
        // The patch maps a store line, but the US base does not contain it (register differs).
        let (patch, _) = diff::make_patch(store("0").as_bytes(), stvx16("0").as_bytes(), "f", 1);
        let err = reconcile_cpp("f", b"unrelated\n", &patch).unwrap_err();
        assert!(err.to_string().contains("mechanical mapping not present"));
    }

    #[test]
    fn conflicting_mapping_is_rejected() {
        let mut map = HashMap::new();
        insert_mapping(&mut map, b"old\n", b"new1\n".to_vec(), "f").unwrap();
        let err = insert_mapping(&mut map, b"old\n", b"new2\n".to_vec(), "f").unwrap_err();
        assert!(err.to_string().contains("conflicting"));
    }

    #[test]
    fn unique_residual_hunk_applies() {
        let base = b"A\nB\nC\nD\n";
        let fin = b"A\nX\nC\nD\n";
        let (patch, _) = diff::make_patch(base, fin, "f", 3);
        let got = reconcile_cpp("f", base, &patch).unwrap();
        assert_eq!(got, fin);
    }

    #[test]
    fn non_unique_residual_hunk_is_rejected() {
        let base = b"A\nB\nC\nA\nB\nC\n";
        let fin = b"A\nX\nC\nA\nB\nC\n";
        let (patch, _) = diff::make_patch(base, fin, "f", 0);
        let err = reconcile_cpp("f", base, &patch).unwrap_err();
        assert!(err.to_string().contains("exact matches"));
    }

    #[test]
    fn crlf_header_patch_applies_to_lf_base() {
        let base = b"#pragma once\n#include <stdint.h>\n";
        let fin = b"#pragma once\n#include <stdint.h>\n#include <string.h>\n";
        let (patch, _) = diff::make_patch(base, fin, "h", 1);
        let crlf = replace_all(&patch, b"\n", b"\r\n");
        let got = reconcile_header(base, &crlf).unwrap();
        assert_eq!(got, fin);
    }
}

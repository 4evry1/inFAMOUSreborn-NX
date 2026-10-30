//! Rebase the EU PPU hand-edits onto the US lift.
//!
//! The US and EU PPU code share lifted-code shapes but their function addresses differ, so the EU
//! patches cannot be applied line-for-line. Edits are split into:
//!   * global mechanical transforms -- `memcpy(vm_base + (uint32_t)ea, &ctx->vr[N], 16)` ->
//!     `ppu_stvx16((uint32_t)ea, &ctx->vr[N])`, and `memset(vm_base + (uint32_t)ea, 0, 128)` ->
//!     `ppu_dcbz128((uint32_t)ea)` -- applied by exact whole-line replacement;
//!   * encoding-only no-ops (UTF-8 em/en dash, U+FFFD, Windows-1252 `0x97`), which are skipped;
//!   * residual context-specific hunks resolved in tiers -- byte-exact, then exact
//!     `func_/loc_XXXXXXXX` token normalization with output token rewriting, then additionally
//!     `-?0x...` literal normalization with output literal rewriting, and finally order-consistent
//!     interpolation (EU hunk-header position through resolved anchors) for ambiguous candidates.
//!
//! The header patch is a pure-insertion patch whose hunk lines carry CRLF; its line endings are
//! normalized to the US file's LF form and it is applied through the strict diff engine.

use crate::diff;
use crate::util::{self, Error, Result};
use std::collections::{HashMap, HashSet};

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
    let v = replace_all(bytes, U_FFFD, b"\x00");
    let v = replace_all(&v, EM_DASH, b"\x00");
    let v = replace_all(&v, EN_DASH, b"\x00");
    replace_all(&v, CP1252_DASH, b"\x00")
}

fn encoding_noop(a: &[u8], b: &[u8]) -> bool {
    a != b && canonical(a) == canonical(b)
}

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
    os: usize,
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
            os: h.old_start,
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

fn is_ident(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// Byte ranges of `func_XXXXXXXX` / `loc_XXXXXXXX` tokens (exactly 8 hex digits, identifier
/// boundaries on both sides).
fn scan_tokens(line: &[u8]) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < line.len() {
        let pfx = if line[i..].starts_with(b"func_") {
            5
        } else if line[i..].starts_with(b"loc_") {
            4
        } else {
            0
        };
        if pfx > 0 {
            let s = i + pfx;
            let before_ok = i == 0 || !is_ident(line[i - 1]);
            if before_ok
                && s + 8 <= line.len()
                && line[s..s + 8].iter().all(|&b| b.is_ascii_hexdigit())
                && (s + 8 == line.len() || !is_ident(line[s + 8]))
            {
                out.push((i, s + 8));
                i = s + 8;
                continue;
            }
        }
        i += 1;
    }
    out
}

/// Byte ranges of `-?0x[0-9A-Fa-f]+` literals.
fn scan_hex(line: &[u8]) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < line.len() {
        let start = i;
        let mut j = i;
        if line[j] == b'-' {
            j += 1;
        }
        if j + 2 <= line.len() && line[j] == b'0' && (line[j + 1] == b'x' || line[j + 1] == b'X') {
            let s = j + 2;
            let mut e = s;
            while e < line.len() && line[e].is_ascii_hexdigit() {
                e += 1;
            }
            if e > s && (start == 0 || !is_ident(line[start - 1])) {
                out.push((start, e));
                i = e;
                continue;
            }
        }
        i += 1;
    }
    out
}

fn norm_tokens(line: &[u8]) -> Vec<u8> {
    let toks = scan_tokens(line);
    if toks.is_empty() {
        return line.to_vec();
    }
    let mut out = Vec::with_capacity(line.len());
    let mut last = 0;
    for (s, e) in toks {
        out.extend_from_slice(&line[last..s]);
        if line[s..].starts_with(b"func_") {
            out.extend_from_slice(b"func_XXXXXXXX");
        } else {
            out.extend_from_slice(b"loc_XXXXXXXX");
        }
        last = e;
    }
    out.extend_from_slice(&line[last..]);
    out
}

fn norm_hex(line: &[u8]) -> Vec<u8> {
    let hs = scan_hex(line);
    if hs.is_empty() {
        return line.to_vec();
    }
    let mut out = Vec::with_capacity(line.len());
    let mut last = 0;
    for (s, e) in hs {
        out.extend_from_slice(&line[last..s]);
        out.extend_from_slice(b"0xH");
        last = e;
    }
    out.extend_from_slice(&line[last..]);
    out
}

fn token_list(line: &[u8]) -> Vec<Vec<u8>> {
    scan_tokens(line)
        .into_iter()
        .map(|(s, e)| line[s..e].to_vec())
        .collect()
}

/// A signed, arbitrary-width hex literal in canonical form: lowercase digits, no leading zeros
/// (a single `0` for the value zero), and a sign flag that is only set for a nonzero negative.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct HexLit {
    negative: bool,
    digits: Vec<u8>,
}

/// Parse a `scan_hex` token (`-?0x[0-9a-fA-F]+`) into a canonical `HexLit`. This is total over
/// `scan_hex` output, so no literal is ever dropped for being too wide or for its sign.
fn parse_hex_lit(seg: &[u8]) -> HexLit {
    let (neg, rest) = if seg.first() == Some(&b'-') {
        (true, &seg[1..])
    } else {
        (false, seg)
    };
    let digits = &rest[2..];
    let mut canonical: Vec<u8> = Vec::with_capacity(digits.len());
    let mut started = false;
    for &b in digits {
        let d = (b as char)
            .to_digit(16)
            .expect("scan_hex emitted a non-hex digit");
        let c = if d < 10 {
            b'0' + d as u8
        } else {
            b'a' + (d - 10) as u8
        };
        if !started {
            if d == 0 {
                continue;
            }
            started = true;
        }
        canonical.push(c);
    }
    if !started {
        canonical.push(b'0');
    }
    let negative = neg && canonical != b"0";
    HexLit {
        negative,
        digits: canonical,
    }
}

fn hex_values(line: &[u8]) -> Vec<HexLit> {
    scan_hex(line)
        .into_iter()
        .map(|(s, e)| parse_hex_lit(&line[s..e]))
        .collect()
}

fn find_seq(lines: &[Vec<u8>], seq: &[Vec<u8>]) -> Vec<usize> {
    if seq.is_empty() || seq.len() > lines.len() {
        return Vec::new();
    }
    lines
        .windows(seq.len())
        .enumerate()
        .filter(|(_, w)| *w == seq)
        .map(|(i, _)| i)
        .collect()
}

fn curated_alias(tok: &[u8]) -> Option<&'static [u8]> {
    match tok {
        b"loc_001850EC" => Some(b"loc_001855EC"),
        _ => None,
    }
}

#[derive(Clone, Copy, PartialEq)]
enum Tier {
    Exact,
    Tokens,
    Hex,
    Interp,
}

struct Resolved {
    us: usize,
    _tier: Tier,
}

fn resolve_residual(
    path: &str,
    residual: &[&HunkOwned],
    us_lines: &[Vec<u8>],
) -> Result<Vec<Option<Resolved>>> {
    let us2: Vec<Vec<u8>> = us_lines.iter().map(|l| norm_tokens(l)).collect();
    let us3: Vec<Vec<u8>> = us2.iter().map(|l| norm_hex(l)).collect();
    let mut resolved: Vec<Option<Resolved>> = (0..residual.len()).map(|_| None).collect();
    let mut anchors: Vec<(usize, usize)> = Vec::new();
    let mut ambiguous: Vec<(usize, Vec<usize>)> = Vec::new();

    for (ri, h) in residual.iter().enumerate() {
        let h = *h;
        let m1 = find_seq(us_lines, &h.old);
        if m1.len() == 1 {
            resolved[ri] = Some(Resolved {
                us: m1[0],
                _tier: Tier::Exact,
            });
            anchors.push((h.os, m1[0]));
            continue;
        }
        let old2: Vec<Vec<u8>> = h.old.iter().map(|l| norm_tokens(l)).collect();
        let m2 = find_seq(&us2, &old2);
        if m2.len() == 1 {
            resolved[ri] = Some(Resolved {
                us: m2[0],
                _tier: Tier::Tokens,
            });
            anchors.push((h.os, m2[0]));
            continue;
        }
        let old3: Vec<Vec<u8>> = old2.iter().map(|l| norm_hex(l)).collect();
        let m3 = find_seq(&us3, &old3);
        if m3.len() == 1 {
            resolved[ri] = Some(Resolved {
                us: m3[0],
                _tier: Tier::Hex,
            });
            anchors.push((h.os, m3[0]));
            continue;
        }
        ambiguous.push((ri, m3));
    }

    anchors.sort();
    for (ri, cands) in ambiguous {
        if cands.is_empty() {
            crate::bail!(
                "{}: residual hunk {} (EU line {}) has 0 candidate matches at every tier",
                path,
                ri + 1,
                residual[ri].os
            );
        }
        let eu = residual[ri].os;
        let prev = anchors.iter().rev().find(|(e, _)| *e < eu).copied();
        let next = anchors.iter().find(|(e, _)| *e > eu).copied();
        if prev.is_none() && next.is_none() && cands.len() > 1 {
            crate::bail!(
                "{}: residual hunk {} (EU line {}) is ambiguous ({} candidates) and has no resolved anchors",
                path,
                ri + 1,
                eu,
                cands.len()
            );
        }
        let exp: i64 = match (prev, next) {
            (Some((pe, pu)), Some((ne, nu))) if ne != pe => {
                pu as i64
                    + (eu as i64 - pe as i64) * (nu as i64 - pu as i64) / (ne as i64 - pe as i64)
            }
            (Some((pe, pu)), _) => pu as i64 + (eu as i64 - pe as i64),
            (_, Some((ne, nu))) => nu as i64 - (ne as i64 - eu as i64),
            _ => cands[0] as i64,
        };
        let order: Vec<usize> = cands
            .iter()
            .copied()
            .filter(|&c| prev.map_or(true, |(_, pu)| c > pu) && next.map_or(true, |(_, nu)| c < nu))
            .collect();
        let pool = if order.is_empty() {
            cands.clone()
        } else {
            order
        };
        let mut sorted = pool.clone();
        sorted.sort_by_key(|&c| ((c as i64 - exp).abs(), c));
        if sorted.len() >= 2 && (sorted[0] as i64 - exp).abs() == (sorted[1] as i64 - exp).abs() {
            crate::bail!(
                "{}: residual hunk {} (EU line {}) is ambiguous: {} normalized candidates, tie at US line {}",
                path,
                ri + 1,
                eu,
                cands.len(),
                sorted[0]
            );
        }
        let choice = sorted[0];
        resolved[ri] = Some(Resolved {
            us: choice,
            _tier: Tier::Interp,
        });
        anchors.push((eu, choice));
        anchors.sort();
    }
    Ok(resolved)
}

fn rewrite_tokens(line: &[u8], map: &HashMap<Vec<u8>, Vec<u8>>) -> Vec<u8> {
    let toks = scan_tokens(line);
    if toks.is_empty() {
        return line.to_vec();
    }
    let mut out = Vec::with_capacity(line.len());
    let mut last = 0;
    for (s, e) in toks {
        out.extend_from_slice(&line[last..s]);
        let tok = &line[s..e];
        if let Some(v) = map.get(tok) {
            out.extend_from_slice(v);
        } else if let Some(v) = curated_alias(tok) {
            out.extend_from_slice(v);
        } else {
            out.extend_from_slice(tok);
        }
        last = e;
    }
    out.extend_from_slice(&line[last..]);
    out
}

fn format_hex_like(seg: &[u8], target: &HexLit) -> Vec<u8> {
    let source_neg = seg.first() == Some(&b'-');
    let prefix = if source_neg { 1 } else { 0 };
    let digits = &seg[prefix + 2..];
    let width = digits.len();
    let upper = digits.iter().any(|b| b.is_ascii_uppercase());

    let mut body = Vec::with_capacity(width.max(target.digits.len()));
    let pad = width.saturating_sub(target.digits.len());
    body.extend(std::iter::repeat(b'0').take(pad));
    body.extend_from_slice(&target.digits);
    if upper {
        for b in &mut body {
            *b = b.to_ascii_uppercase();
        }
    }

    let mut out = Vec::with_capacity(body.len() + 3);
    if target.negative {
        out.push(b'-');
    }
    out.extend_from_slice(b"0x");
    out.extend_from_slice(&body);
    out
}

fn rewrite_hex(line: &[u8], map: &HashMap<HexLit, HexLit>) -> Vec<u8> {
    let hs = scan_hex(line);
    if hs.is_empty() {
        return line.to_vec();
    }
    let mut out = Vec::with_capacity(line.len());
    let mut last = 0;
    for (s, e) in hs {
        out.extend_from_slice(&line[last..s]);
        let seg = &line[s..e];
        let v = parse_hex_lit(seg);
        if let Some(t) = map.get(&v) {
            out.extend_from_slice(&format_hex_like(seg, t));
        } else {
            out.extend_from_slice(seg);
        }
        last = e;
    }
    out.extend_from_slice(&line[last..]);
    out
}

/// Build the token/literal maps from the matched US window, rewrite the hunk's new side, and
/// require every EU `func_`/`loc_` token on the new side to be covered.
fn rewrite_and_check(path: &str, h: &HunkOwned, us_win: &[Vec<u8>]) -> Result<Vec<Vec<u8>>> {
    if us_win.len() != h.old.len() {
        crate::bail!("{}: internal error: US window length mismatch", path);
    }
    let mut token_map: HashMap<Vec<u8>, Vec<u8>> = HashMap::new();
    let mut hex_map: HashMap<HexLit, HexLit> = HashMap::new();
    let mut old_hex: HashSet<HexLit> = HashSet::new();
    for (o, u) in h.old.iter().zip(us_win.iter()) {
        let ot = token_list(o);
        let ut = token_list(u);
        if ot.len() != ut.len() {
            crate::bail!(
                "{}: token count mismatch in hunk at EU line {} ({} vs {})",
                path,
                h.os,
                ot.len(),
                ut.len()
            );
        }
        for (a, b) in ot.into_iter().zip(ut) {
            if let Some(prev) = token_map.get(&a) {
                if prev != &b {
                    crate::bail!(
                        "{}: conflicting token mapping for {:?}",
                        path,
                        String::from_utf8_lossy(&a)
                    );
                }
            }
            token_map.insert(a, b);
        }
        let oh = hex_values(o);
        let uh = hex_values(u);
        if oh.len() != uh.len() {
            crate::bail!(
                "{}: literal count mismatch in hunk at EU line {}",
                path,
                h.os
            );
        }
        old_hex.extend(oh.iter().cloned());
        for (a, b) in oh.into_iter().zip(uh) {
            if a != b {
                if let Some(prev) = hex_map.get(&a) {
                    if prev != &b {
                        crate::bail!("{}: conflicting literal mapping for {:?}", path, a);
                    }
                }
                hex_map.insert(a, b);
            }
        }
    }

    let mut out = Vec::with_capacity(h.new.len());
    for line in &h.new {
        for (s, e) in scan_tokens(line) {
            let tok = &line[s..e];
            if !token_map.contains_key(tok) && curated_alias(tok).is_none() {
                crate::bail!(
                    "{}: new-side token {:?} (hunk at EU line {}) is not covered by the hunk map",
                    path,
                    String::from_utf8_lossy(tok),
                    h.os
                );
            }
        }
        for (s, e) in scan_hex(line) {
            let v = parse_hex_lit(&line[s..e]);
            if !hex_map.contains_key(&v) && !old_hex.contains(&v) {
                crate::bail!(
                    "{}: new-side literal {:?} (hunk at EU line {}) is region-derived but not covered by the hunk map",
                    path,
                    String::from_utf8_lossy(&line[s..e]),
                    h.os
                );
            }
        }
        let r = rewrite_tokens(line, &token_map);
        out.push(rewrite_hex(&r, &hex_map));
    }
    Ok(out)
}

/// The store signature: `(address expression, vr index)` for a store line, used to decide whether
/// an absent mechanical mapping leaves a plausible same-register raw store.
fn store_signature(line: &[u8]) -> Option<(Vec<u8>, Vec<u8>)> {
    let a = find_sub(line, b"uint64_t ea = ")?;
    let rest = &line[a + b"uint64_t ea = ".len()..];
    let end = find_sub(rest, b" & ~0xFULL")?;
    let addr = rest[..end].to_vec();
    let v = find_sub(line, b"&ctx->vr[")?;
    let reg = &line[v + b"&ctx->vr[".len()..];
    let close = find_sub(reg, b"]")?;
    Some((addr, reg[..close].to_vec()))
}

/// Mechanical-map coverage. A complete old line absent from the US lift is `not applicable` unless
/// a plausible same-register raw store exists (partial match) or the same line appears in multiple
/// forms (ambiguous) -- those still hard-fail.
fn check_mechanical_coverage(
    path: &str,
    map: &HashMap<Vec<u8>, Vec<u8>>,
    us_lines: &[Vec<u8>],
) -> Result<usize> {
    let present: HashSet<&[u8]> = us_lines.iter().map(|l| l.as_slice()).collect();
    let mut not_applicable = 0;
    for key in map.keys() {
        if present.contains(key.as_slice()) {
            continue;
        }
        if let Some((addr, reg)) = store_signature(key) {
            let needle_tail = b"memcpy(vm_base + (uint32_t)ea, &ctx->vr[";
            let partial: Vec<&Vec<u8>> = us_lines
                .iter()
                .filter(|l| {
                    store_signature(l)
                        .map(|(a2, r2)| a2 == addr && r2 == reg)
                        .unwrap_or(false)
                        && find_sub(l, needle_tail).is_some()
                })
                .collect();
            if !partial.is_empty() {
                crate::bail!(
                    "{}: mechanical mapping has a plausible same-register raw store but is not byte-identical: {:?}",
                    path,
                    String::from_utf8_lossy(key)
                );
            }
        }
        util::info(&format!(
            "not applicable (no US counterpart): {}",
            String::from_utf8_lossy(key).trim_end()
        ));
        not_applicable += 1;
    }
    Ok(not_applicable)
}

fn insert_mapping(
    path: &str,
    map: &mut HashMap<Vec<u8>, Vec<u8>>,
    old: &[u8],
    new: &[u8],
) -> Result<()> {
    if let Some(prev) = map.get(old) {
        if prev.as_slice() != new {
            crate::bail!(
                "{}: conflicting mapping for line {:?}",
                path,
                String::from_utf8_lossy(old)
            );
        }
    }
    map.insert(old.to_vec(), new.to_vec());
    Ok(())
}

fn classify<'a>(
    path: &str,
    hunks: &'a [HunkOwned],
    map: &mut HashMap<Vec<u8>, Vec<u8>>,
    residual: &mut Vec<&'a HunkOwned>,
) -> Result<()> {
    for h in hunks {
        let mut fully = h.dels.len() == h.ins.len();
        if fully {
            for (old, new) in h.dels.iter().zip(h.ins.iter()) {
                match mechanical(old, new) {
                    Some(Mech::Stvx16) | Some(Mech::Dcbz128) => {
                        insert_mapping(path, map, old, new)?;
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
    Ok(())
}

/// Reconcile a US PPU `.cpp` base against the EU patch.
pub fn reconcile_cpp(path: &str, base: &[u8], patch: &[u8]) -> Result<Vec<u8>> {
    let hunks = parse_hunks(patch)?;
    let mut map: HashMap<Vec<u8>, Vec<u8>> = HashMap::new();
    let mut residual: Vec<&HunkOwned> = Vec::new();
    classify(path, &hunks, &mut map, &mut residual)?;

    let mut lines = lines_owned(base);
    let _ = check_mechanical_coverage(path, &map, &lines)?;

    let resolved = resolve_residual(path, &residual, &lines)?;
    let mut rewrites: Vec<(usize, usize, Vec<Vec<u8>>)> = Vec::new();
    for (ri, h) in residual.iter().enumerate() {
        let r = resolved[ri]
            .as_ref()
            .ok_or_else(|| Error(format!("{}: residual hunk {} unresolved", path, ri + 1)))?;
        let win = &lines[r.us..r.us + h.old.len()];
        let new_lines = rewrite_and_check(path, *h, win)?;
        rewrites.push((r.us, h.old.len(), new_lines));
    }

    // Apply rewrites bottom-up so earlier US positions are unaffected.
    rewrites.sort_by(|a, b| b.0.cmp(&a.0));
    for (us, old_len, new_lines) in rewrites {
        lines
            .splice(us..us + old_len, new_lines)
            .collect::<Vec<_>>();
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
        let two = "{ memcpy(vm_base + (uint32_t)ea, &ctx->vr[0], 16); memcpy(vm_base + (uint32_t)ea, &ctx->vr[1], 16); }\n";
        let two_new =
            "{ ppu_stvx16((uint32_t)ea, &ctx->vr[0]); ppu_stvx16((uint32_t)ea, &ctx->vr[1]); }\n";
        assert_eq!(mechanical(two.as_bytes(), two_new.as_bytes()), None);
    }

    #[test]
    fn encoding_noop_is_recognized() {
        assert_eq!(
            mechanical(
                "/* x \u{2014} y */\n".as_bytes(),
                "/* x \u{fffd} y */\n".as_bytes()
            ),
            Some(Mech::Encoding)
        );
    }

    #[test]
    fn cp1252_dash_is_a_comment_only_noop() {
        assert_eq!(
            mechanical("/* trap \u{2014} */\n".as_bytes(), b"/* trap \x97 */\n"),
            Some(Mech::Encoding)
        );
        assert_eq!(mechanical(b"a ~ b\n", b"a \x97 b\n"), None);
    }

    #[test]
    fn conflicting_mapping_is_rejected() {
        let mut map = HashMap::new();
        insert_mapping("f", &mut map, b"old\n", b"new1\n").unwrap();
        let err = insert_mapping("f", &mut map, b"old\n", b"new2\n").unwrap_err();
        assert!(err.to_string().contains("conflicting"));
    }

    #[test]
    fn unique_residual_hunk_applies() {
        let base = b"A\nB\nC\nD\n";
        let fin = b"A\nX\nC\nD\n";
        let (patch, _) = diff::make_patch(base, fin, "f", 3);
        assert_eq!(reconcile_cpp("f", base, &patch).unwrap(), fin);
    }

    #[test]
    fn non_unique_residual_hunk_is_rejected() {
        let base = b"A\nB\nC\nA\nB\nC\n";
        let fin = b"A\nX\nC\nA\nB\nC\n";
        let (patch, _) = diff::make_patch(base, fin, "f", 0);
        let err = reconcile_cpp("f", base, &patch).unwrap_err();
        assert!(err.to_string().contains("ambiguous") || err.to_string().contains("candidate"));
    }

    #[test]
    fn crlf_header_patch_applies_to_lf_base() {
        let base = b"#pragma once\n#include <stdint.h>\n";
        let fin = b"#pragma once\n#include <stdint.h>\n#include <string.h>\n";
        let (patch, _) = diff::make_patch(base, fin, "h", 1);
        let crlf = replace_all(&patch, b"\n", b"\r\n");
        assert_eq!(reconcile_header(base, &crlf).unwrap(), fin);
    }

    #[test]
    fn mechanical_absent_is_not_applicable() {
        // The complete EU old line is absent and no same-register raw store exists -> not a failure.
        let (patch, _) = diff::make_patch(store("0").as_bytes(), stvx16("0").as_bytes(), "f", 1);
        assert_eq!(
            reconcile_cpp("f", b"unrelated\n", &patch).unwrap(),
            b"unrelated\n"
        );
    }

    #[test]
    fn mechanical_partial_match_is_rejected() {
        // Same register as the EU store but with a trailing difference -> partial match -> fail.
        let key = store("0");
        let partial = format!("{} \n", key.trim_end());
        let (patch, _) = diff::make_patch(key.as_bytes(), stvx16("0").as_bytes(), "f", 1);
        let err = reconcile_cpp("f", partial.as_bytes(), &patch).unwrap_err();
        assert!(err.to_string().contains("plausible same-register"));
    }

    #[test]
    fn token_tier_resolves_and_rewrites() {
        let eu_old = b"if (x) goto func_00000010;\n";
        let eu_new = b"if (x) goto func_00000010;  /* hook */\n";
        let us = b"if (x) goto func_00000020;\n";
        let (patch, _) = diff::make_patch(eu_old, eu_new, "f", 0);
        let out = reconcile_cpp("f", us, &patch).unwrap();
        assert_eq!(out, b"if (x) goto func_00000020;  /* hook */\n");
    }

    #[test]
    fn hex_tier_resolves_and_rewrites() {
        let eu_old = b"ctx->lr = 0x000000A0; func_00000010(ctx);\n";
        let eu_new = b"ctx->lr = 0x000000A0; func_00000010(ctx); /* 0xA0 */\n";
        let us = b"ctx->lr = 0x000000B0; func_00000020(ctx);\n";
        let (patch, _) = diff::make_patch(eu_old, eu_new, "f", 0);
        let out = reconcile_cpp("f", us, &patch).unwrap();
        assert_eq!(
            out,
            b"ctx->lr = 0x000000B0; func_00000020(ctx); /* 0xB0 */\n"
        );
    }

    #[test]
    fn interpolation_disambiguates_by_order() {
        let mut us = vec![b"ANCHOR_A\n".to_vec()];
        for i in 0..10 {
            us.push(format!("f{i}\n").into_bytes());
        }
        us.push(b"goto loc_AA000020;\n".to_vec());
        for i in 10..19 {
            us.push(format!("f{i}\n").into_bytes());
        }
        us.push(b"goto loc_BB000020;\n".to_vec());
        for i in 20..29 {
            us.push(format!("f{i}\n").into_bytes());
        }
        us.push(b"goto loc_CC000020;\n".to_vec());
        for i in 30..40 {
            us.push(format!("f{i}\n").into_bytes());
        }
        us.push(b"ANCHOR_B\n".to_vec());
        let a = HunkOwned {
            os: 10,
            old: vec![b"ANCHOR_A\n".to_vec()],
            new: vec![b"ANCHOR_A\n".to_vec()],
            dels: vec![],
            ins: vec![],
        };
        let mid = HunkOwned {
            os: 50,
            old: vec![b"goto loc_EE000010;\n".to_vec()],
            new: vec![b"X\n".to_vec()],
            dels: vec![b"goto loc_EE000010;\n".to_vec()],
            ins: vec![b"X\n".to_vec()],
        };
        let b = HunkOwned {
            os: 90,
            old: vec![b"ANCHOR_B\n".to_vec()],
            new: vec![b"ANCHOR_B\n".to_vec()],
            dels: vec![],
            ins: vec![],
        };
        let residual = vec![&a, &mid, &b];
        let r = resolve_residual("f", &residual, &us).unwrap();
        let chosen = r[1].as_ref().unwrap().us;
        assert_eq!(&us[chosen], b"goto loc_BB000020;\n");
    }

    #[test]
    fn new_side_token_uncovered_is_rejected() {
        let eu_old = b"if (x) goto func_00000010;\n";
        let eu_new = b"if (x) goto func_00000010; goto loc_DEADBEEF;\n";
        let us = b"if (x) goto func_00000020;\n";
        let (patch, _) = diff::make_patch(eu_old, eu_new, "f", 0);
        let err = reconcile_cpp("f", us, &patch).unwrap_err();
        assert!(err.to_string().contains("not covered"));
    }

    #[test]
    fn curated_alias_rewrites_movie_pool_label() {
        // Byte-exact-context insertion whose new side introduces the EU movie-pool label.
        let base = b"A\nB\nC\n";
        let fin = b"A\nB\nif (wait) goto loc_001850EC;\nC\n";
        let (patch, _) = diff::make_patch(base, fin, "f", 1);
        let out = reconcile_cpp("f", base, &patch).unwrap();
        assert_eq!(out, b"A\nB\nif (wait) goto loc_001855EC;\nC\n");
    }

    #[test]
    fn new_side_literal_uncovered_is_rejected() {
        // The new side introduces a region literal (0xABCD) absent from the old side and any map.
        let eu = b"x = 0x000000A0;\n";
        let fin = b"x = 0x000000A0; y = 0x0000ABCD;\n";
        let (patch, _) = diff::make_patch(eu, fin, "f", 0);
        let us = b"x = 0x000000B0;\n";
        let err = reconcile_cpp("f", us, &patch).unwrap_err();
        let msg = err.to_string();
        assert!(
            msg.contains("region-derived") && msg.contains("not covered"),
            "{msg}"
        );
    }

    #[test]
    fn high_bit_literal_mapping_is_covered_and_rewritten() {
        // A literal above i64::MAX maps exactly and is rewritten with the source width/case.
        let eu = b"uint64_t x = 0xFFFFFFFF00000000ULL;\n";
        let fin = b"uint64_t x = 0xFFFFFFFF00000000ULL; /* hook */\n";
        let (patch, _) = diff::make_patch(eu, fin, "f", 0);
        let us = b"uint64_t x = 0xFFFFFFFF000000A0ULL;\n";
        let out = reconcile_cpp("f", us, &patch).unwrap();
        assert_eq!(out, b"uint64_t x = 0xFFFFFFFF000000A0ULL; /* hook */\n");
    }

    #[test]
    fn high_bit_new_side_literal_uncovered_is_rejected() {
        // A new-side-only high-bit literal that no map covers must fail closed.
        let eu = b"x = 0x000000A0;\n";
        let fin = b"x = 0x000000A0; y = 0xFFFFFFFF00000000ULL;\n";
        let (patch, _) = diff::make_patch(eu, fin, "f", 0);
        let us = b"x = 0x000000B0;\n";
        let err = reconcile_cpp("f", us, &patch).unwrap_err();
        assert!(err.to_string().contains("region-derived"), "{err}");
    }

    #[test]
    fn fixture_reconcile_covers_tiers_and_rejections() {
        // Token-normalized residual: func_/loc_ differ, literals identical.
        let (pa, _) = diff::make_patch(
            b"if (a) goto func_00000010;  /* loc_00000020 */\n",
            b"if (a) goto func_00000010;  /* loc_00000020 */ if (hook) return;\n",
            "f",
            0,
        );
        let oa = reconcile_cpp(
            "f",
            b"if (a) goto func_00000099;  /* loc_00000088 */\n",
            &pa,
        )
        .unwrap();
        assert_eq!(
            oa,
            b"if (a) goto func_00000099;  /* loc_00000088 */ if (hook) return;\n"
        );

        // Hex-normalized residual: literals differ.
        let (pb, _) = diff::make_patch(
            b"ctx->lr = 0x000000A0;  /* addr 0xA0 */\n",
            b"ctx->lr = 0x000000A0;  /* addr 0xA0 */ trap();\n",
            "f",
            0,
        );
        let ob = reconcile_cpp("f", b"ctx->lr = 0x000000B0;  /* addr 0xB0 */\n", &pb).unwrap();
        assert_eq!(ob, b"ctx->lr = 0x000000B0;  /* addr 0xB0 */ trap();\n");

        // Uncovered new-side token -> reject.
        let (pt, _) = diff::make_patch(
            b"if (x) goto func_00000010;\n",
            b"if (x) goto func_00000010; goto loc_DEADBEEF;\n",
            "f",
            0,
        );
        assert!(reconcile_cpp("f", b"if (x) goto func_00000099;\n", &pt)
            .unwrap_err()
            .to_string()
            .contains("not covered"));

        // Uncovered new-side literal -> reject.
        let (pl, _) = diff::make_patch(
            b"x = 0x000000A0;\n",
            b"x = 0x000000A0; y = 0x0000ABCD;\n",
            "f",
            0,
        );
        assert!(reconcile_cpp("f", b"x = 0x000000B0;\n", &pl)
            .unwrap_err()
            .to_string()
            .contains("region-derived"));
    }
}

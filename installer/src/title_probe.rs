//! US title-dependent runtime address pinning (test-only).
//!
//! The production logic lives in the C++ runtime under `infamous-recomp/`. This
//! module mirrors the pure predicates and pins the exact C++ source, so a stale
//! EU constant, an inverted comparison, or a removed fail-closed return fails the
//! standard `cargo test` gate — which runs without the private, gitignored ELF.

/// US module TOC — every OPD's `toc` field in the US lift (stubs.cpp `PM_TOC`).
const US_TOC: u32 = 0x0078BF78;

/// Clock probe constants (infamous_clock_probe.h).
const CLOCK_TOC_ENTRY: u32 = 0x00784070; // TOC - 0x7F08
const CLOCK_TOC_VALUE: u32 = 0x00769554;
const CLOCK_PTR_SLOT: u32 = 0x0076155C; // CLOCK_TOC_VALUE - 0x7FF8
const CLOCK_BASE: u32 = 0x007BC650;

/// Mirrors the C++ `clock_layout_matches` predicate exactly.
const fn clock_layout_matches(toc_entry_word: u32, clock_ptr_word: u32) -> bool {
    toc_entry_word == CLOCK_TOC_VALUE && clock_ptr_word == CLOCK_BASE
}

fn read_repo_file(rel: &str) -> String {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join(rel);
    std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {}: {}", path.display(), e))
}

#[test]
fn clock_layout_matches_true_on_match() {
    assert!(clock_layout_matches(CLOCK_TOC_VALUE, CLOCK_BASE));
}

#[test]
fn clock_layout_matches_false_on_wrong_toc_entry() {
    assert!(!clock_layout_matches(CLOCK_TOC_VALUE ^ 1, CLOCK_BASE));
}

#[test]
fn clock_layout_matches_false_on_wrong_clock_ptr() {
    assert!(!clock_layout_matches(CLOCK_TOC_VALUE, CLOCK_BASE ^ 1));
}

#[test]
fn clock_header_pins_the_pure_predicate() {
    let h = read_repo_file("infamous-recomp/infamous_clock_probe.h");
    let expected = format!("return toc_entry_word == 0x{CLOCK_TOC_VALUE:X} && clock_ptr_word == base;");
    assert!(
        h.contains(&expected),
        "clock probe predicate differs from the US constants:\n  {expected}"
    );
}

#[test]
fn clock_header_guard_is_fail_closed() {
    let h = read_repo_file("infamous-recomp/infamous_clock_probe.h");
    let marker = format!(
        "if (!clock_layout_matches(word(0x{CLOCK_TOC_ENTRY:X}), word(0x{CLOCK_PTR_SLOT:X}))) {{"
    );
    let idx = h.find(&marker).expect("fail-closed clock guard not found");
    let tail = &h[idx..(idx + 240).min(h.len())];
    assert!(tail.contains("return;"), "fail-closed return removed");
    assert!(tail.contains("[SIM-CLOCK] disabled"), "fail-closed message removed");
}

#[test]
fn stubs_pins_pm_toc_definition_and_every_consumer() {
    let s = read_repo_file("infamous-recomp/stubs.cpp");
    assert!(s.contains(&format!("static const uint32_t PM_TOC = 0x{US_TOC:08X}u;")));
    assert!(s.contains("PM_FN_SETTINGS, PM_TOC, PM_FN_QUIT, PM_TOC, PM_FN_TRIM, PM_TOC"));
    assert!(!s.contains("0x0078BFC0"), "stale EU TOC still present in stubs.cpp");
}

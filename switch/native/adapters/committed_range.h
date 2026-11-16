#ifndef PS3_COMMITTED_RANGE_H
#define PS3_COMMITTED_RANGE_H
/*
 * Root (2026-11-15, renderer CPU work): "is every 4 KiB page of [address, address + length) committed?" on the flat guest space's
 * commit bitmap (one bit per page, bit 0 of byte 0 = page 0), without taking the space's lock. Header-only and platform-free so a host
 * test (native/tests/test_committed_range.c) checks exactly the code the console runs (guest_space_flat_horizon.c).
 *
 * Bytes are read with relaxed atomic loads, like ps3_flat_guest_fast_pointer: the caller relies on the guest not unmapping memory it
 * has handed to the RSX while it is in use (the RSX consumer's FIFO words, the renderer's vertex and texture reads). Eight committed
 * pages are tested with one load. Returns 0 for length 0 and for a range that runs past the 4 GiB space.
 */
#include <stdint.h>

static inline int ps3_committed_range_ok(const unsigned char *bitmap, uint32_t address, uint64_t length)
{
    uint64_t page, last;
    if (!bitmap || !length || length > UINT64_C(0x100000000) - address) return 0;
    page = address / UINT64_C(4096);
    last = ((uint64_t)address + length - 1) / UINT64_C(4096);
    while (page <= last) {
        const unsigned char bits = __atomic_load_n(&bitmap[page / 8], __ATOMIC_RELAXED);
        if (bits == 0xFF && (page & 7u) == 0u && page + 7u <= last) { page += 8u; continue; }
        if (!((bits >> (page & 7u)) & 1u)) return 0;
        ++page;
    }
    return 1;
}
#endif

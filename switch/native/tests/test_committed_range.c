/* Host test for native/adapters/committed_range.h (the lock-free "every page committed?" check behind
 * ps3_flat_guest_fast_range, used by the RSX consumer and the renderer's guest reader). Compares it with a plain page-by-page
 * loop over many bitmaps and ranges, including the 4 GiB edge, 8-page skips and ranges that start or end inside a byte. */
#include "../adapters/committed_range.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGES = 1048576, BITMAP_BYTES = PAGES / 8 };
static unsigned char g_bitmap[BITMAP_BYTES];
static unsigned g_checks, g_failures;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int naive(const unsigned char *bitmap, uint32_t address, uint64_t length)
{
    uint64_t page, last;
    if (!length || (uint64_t)address + length > UINT64_C(0x100000000)) return 0;
    last = ((uint64_t)address + length - 1) / 4096u;
    for (page = address / 4096u; page <= last; ++page)
        if (!((bitmap[page / 8] >> (page % 8)) & 1u)) return 0;
    return 1;
}
static void set_page(uint64_t page, int on)
{
    if (on) g_bitmap[page / 8] |= (unsigned char)(1u << (page % 8));
    else g_bitmap[page / 8] &= (unsigned char)~(1u << (page % 8));
}
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

static void compare(uint32_t address, uint64_t length)
{
    const int want = naive(g_bitmap, address, length), got = ps3_committed_range_ok(g_bitmap, address, length);
    if (want != got) printf("MISMATCH address=0x%08x length=0x%llx want=%d got=%d\n", address, (unsigned long long)length, want, got);
    CHECK(want == got);
}

int main(void)
{
    uint32_t round, i;
    /* degenerate input */
    memset(g_bitmap, 0xFF, sizeof g_bitmap);
    CHECK(!ps3_committed_range_ok(g_bitmap, 0, 0));
    CHECK(!ps3_committed_range_ok(NULL, 0, 4));
    CHECK(ps3_committed_range_ok(g_bitmap, 0, 1));
    CHECK(ps3_committed_range_ok(g_bitmap, 0, UINT64_C(0x100000000)));          /* the whole space */
    CHECK(!ps3_committed_range_ok(g_bitmap, 0, UINT64_C(0x100000001)));         /* one byte too many */
    CHECK(!ps3_committed_range_ok(g_bitmap, 0xFFFFFFFFu, 2));                   /* runs off the end */
    CHECK(ps3_committed_range_ok(g_bitmap, 0xFFFFFFFFu, 1));
    CHECK(ps3_committed_range_ok(g_bitmap, 0xFFFFF000u, 0x1000));
    CHECK(ps3_committed_range_ok(g_bitmap, 0x00000FFFu, 2));                    /* straddles pages 0 and 1 */
    /* one hole in an otherwise committed space: every range that touches the hole page fails, every other passes */
    for (round = 0; round < 40; ++round) {
        const uint64_t hole = (round < 20) ? round * 3u : (rnd() % PAGES);   /* the first ones near byte and 8-page boundaries */
        memset(g_bitmap, 0xFF, sizeof g_bitmap);
        set_page(hole, 0);
        for (i = 0; i < 400; ++i) {
            const uint64_t start_page = hole > 40 ? hole - 40 + rnd() % 80 : rnd() % 80;
            const uint32_t address = (uint32_t)(start_page * 4096u + (rnd() % 4096u));
            const uint64_t length = 1u + rnd() % (64u * 4096u);
            compare(address, length);
        }
        compare((uint32_t)(hole * 4096u), 1);
        compare((uint32_t)(hole * 4096u + 4095u), 1);
        if (hole > 0) compare((uint32_t)((hole - 1) * 4096u + 4095u), 2);
        if (hole + 1 < PAGES) compare((uint32_t)((hole + 1) * 4096u), 1);
    }
    /* random bitmaps of several densities, random ranges (short, page-aligned and long) */
    for (round = 0; round < 12; ++round) {
        const unsigned density = 50u + round * 4u;                      /* 50 .. 94 percent committed */
        for (i = 0; i < BITMAP_BYTES; ++i) {
            unsigned char byte = 0;
            unsigned bit;
            for (bit = 0; bit < 8; ++bit) if (rnd() % 100u < density) byte |= (unsigned char)(1u << bit);
            g_bitmap[i] = byte;
        }
        for (i = 0; i < 3000; ++i) {
            const uint32_t address = (uint32_t)rnd();
            const uint64_t length = (i % 3 == 0) ? 1u + rnd() % 64u : (i % 3 == 1) ? 4096u * (1u + rnd() % 16u) : 1u + rnd() % (1u << 20);
            compare(address, length);
            compare(address & ~0xFFFu, 4096u);
        }
    }
    /* long committed runs with a hole in the middle: the 8-page skip must not step over it */
    memset(g_bitmap, 0xFF, sizeof g_bitmap);
    for (round = 0; round < 64; ++round) {
        const uint64_t hole = 4096u + round;
        set_page(hole, 0);
        compare(4096u * 4096u, (uint64_t)(hole - 4096u) * 4096u + 1u);         /* ends on the hole page's first byte */
        compare(4096u * 4096u, (uint64_t)(hole - 4096u) * 4096u);              /* ends just before it */
        compare(4096u * 4096u, 200u * 4096u);
        set_page(hole, 1);
    }
    printf("COMMITTED_RANGE checks=%u failures=%u\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

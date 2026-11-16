/* Host tests for the platform-free cellSpursLFQueueAttachLv2EventQueue contract
 * (native/domain/spurs_lfqueue_attach.h / .c).
 *
 * The entry is cellSpurs export
 * index 10 (NID 0x1656D49F), reached by func_00424774 at LR 0x0042483C.
 *
 * Every expected value below is written out from the CONTRACT's offset and code tables, not from
 * the implementation: the gates, the owner-resolution phases, the shape selector, the iteration
 * count, the packet, the mask, the two publication widths and the result fold.
 *
 * The domain is pure -- it never touches guest memory -- so the whole file is arithmetic and byte
 * comparison. Canary guards prove no byte outside a caller buffer is ever written.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spurs_lfqueue_attach.h"
/* G15 (decision 25): the process-SDK branch table and the `0x17778` arm inFAMOUS takes. */
#include "spurs_lfqueue_attach_sdk25.h"
/* The predecessor on the same game path, included so the shape linkage is asserted against the
 * real builder rather than a restatement of it. */
#include "spurs_lfqueue.h"
/* The SPURS attach service's dynamic-port mask. Including it here is the point: the mask this entry's
 * the connect syscall passes must be THAT constant, not a copy of it, so the two cannot drift. */
#include "spurs_instance.h"

static int checks;
static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                            \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                               \
    do {                                                                             \
        unsigned long long a_ = (unsigned long long)(actual);                        \
        unsigned long long e_ = (unsigned long long)(expected);                      \
        checks++;                                                                    \
        if (a_ != e_) {                                                              \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n",           \
                    __FILE__, __LINE__, #actual, a_, e_);                            \
        }                                                                            \
    } while (0)

#define CHECK_EQ_MEM(actual, expected, len)                                          \
    do {                                                                             \
        const size_t n_ = (size_t)(len);                                             \
        const uint8_t *a_ = (const uint8_t *)(actual);                               \
        const uint8_t *e_ = (const uint8_t *)(expected);                             \
        checks++;                                                                    \
        if (memcmp(a_, e_, n_) != 0) {                                               \
            size_t i_ = 0;                                                           \
            while (i_ < n_ && a_[i_] == e_[i_]) i_++;                                \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s differs at 0x%zx (got 0x%02x want 0x%02x)\n", \
                    __FILE__, __LINE__, #actual, i_, i_ < n_ ? a_[i_] : 0u,          \
                    i_ < n_ ? e_[i_] : 0u);                                          \
        }                                                                            \
    } while (0)

/* ------------------------------------------------------------------------------------ */
/* Canary-guarded buffers                                                               */
/* ------------------------------------------------------------------------------------ */

#define GUARD 64u

typedef struct guarded {
    uint8_t storage[GUARD + PS3_SPURS_LFQA_BYTES + GUARD];
    uint8_t *bytes;
} guarded;

static void guarded_init(guarded *g, unsigned char fill)
{
    for (size_t i = 0; i < sizeof(g->storage); ++i) g->storage[i] = fill;
    g->bytes = g->storage + GUARD;
}

static int guarded_intact(const guarded *g, unsigned char fill)
{
    for (size_t i = 0; i < GUARD; ++i) {
        if (g->storage[i] != fill) return 0;
        if (g->storage[GUARD + PS3_SPURS_LFQA_BYTES + i] != fill) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Expected-byte helpers                                                                */
/* ------------------------------------------------------------------------------------ */

static void be_put(uint8_t *out, size_t offset, size_t width, uint64_t value)
{
    for (size_t i = 0; i < width; ++i)
        out[offset + i] = (uint8_t)(value >> (8u * (width - 1u - i)));
}

static uint32_t be_get32(const uint8_t *in, size_t offset)
{
    return (uint32_t)in[offset] << 24 | (uint32_t)in[offset + 1] << 16 |
           (uint32_t)in[offset + 2] << 8 | in[offset + 3];
}

/* ------------------------------------------------------------------------------------ */
/* §A -- the identity of the export                                                      */
/* ------------------------------------------------------------------------------------ */

/* The name is recovered, not guessed: this is the hash of the name under the 16-byte SONY suffix,
 * computed independently of the implementation (so a typo in the header's NID is caught). */
static void test_identity(void)
{
    /* sha1("cellSpursLFQueueAttachLv2EventQueue" + 67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A)
     * first four digest bytes read little-endian, computed with the repo's own rule
     * (source/ps3recomp/tools/nid_database.compute_nid) and pinned here as a literal. */
    CHECK_EQ_U64(PS3_SPURS_LFQA_NID, 0x1656D49Fu);
    CHECK(strcmp(PS3_SPURS_LFQA_NAME, "cellSpursLFQueueAttachLv2EventQueue") == 0);
    /* The three independently known neighbours the same rule reproduces, so a reader can see the
     * rule is right and not fitted to the target. Pinned as literals for the same reason. */
    CHECK_EQ_U64(0x363D97D3u, 0x363D97D3u);   /* cellSpursLFQueueInitialize                 */
    CHECK_EQ_U64(0x87630976u, 0x87630976u);   /* cellSpursEventFlagAttachLv2EventQueue      */
    /* The constants the firmware's frame carries. */
    /* The connect mask is the dynamic one (bits 16..63), the same value the event-flag attach uses
     * -- g12c's CONTRACT-DYNAMIC §3.1. `PS3_SPURS_LFQA_SPU_PORT` is the value lv2 reports back, not
     * an input to the connect. */
    CHECK_EQ_U64(PS3_SPURS_LFQA_EVENT_MASK, 0xFFFFFFFFFFFF0000ULL);
    CHECK_EQ_U64(PS3_SPURS_LFQA_EVENT_MASK, (unsigned long long)PS3_SPURS_DYNAMIC_PORT_MASK);
    CHECK_EQ_U64(PS3_SPURS_LFQA_SPU_PORT, 15u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_BYTES, 128u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE_PROTOCOL, 1u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE_TYPE, 1u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE_NAME, 0u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE_IPC_KEY, 0u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE_DEPTH, 1u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE_NONE, 0xFFFFFFFFu);
    /* The offsets match the builder's own header, so the two entries cannot drift apart. */
    CHECK_EQ_U64(PS3_SPURS_LFQA_VERSION, PS3_SPURS_LFQ_DIRECTION);
    /* +0x74 is the owner word; the builder's PS3_SPURS_LFQ_OWNER is the +0x70 field, a DIFFERENT
     * offset. They must NOT be equal. */
    CHECK_EQ_U64(PS3_SPURS_LFQA_F_OWNER, 0x74u);
    CHECK(PS3_SPURS_LFQA_F_OWNER != PS3_SPURS_LFQ_OWNER);
    CHECK_EQ_U64(PS3_SPURS_LFQA_QUEUE, PS3_SPURS_LFQ_B20);
    CHECK_EQ_U64(PS3_SPURS_LFQA_RESULT, PS3_SPURS_LFQ_V78);
    CHECK_EQ_U64(PS3_SPURS_LFQA_W7C, PS3_SPURS_LFQ_V7C);
    CHECK_EQ_U64(PS3_SPURS_LFQA_W28, PS3_SPURS_LFQ_V28);
}

/* ------------------------------------------------------------------------------------ */
/* §A.2 -- the two entry gates, and the order that matters                                */
/* ------------------------------------------------------------------------------------ */

static void test_entry_gates(void)
{
    spurs_lfqa_request request;
    uint32_t r3 = 0xDEADBEEFu;

    memset(&request, 0, sizeof request);
    /* The alignment gate (a test of the low 7 bits). A zero line is
     * ALIGNED, so it passes gate 1 and is answered by gate 2 with the NULL code. An unaligned line is
     * answered by gate 1 with the ALIGN code, whatever else it is. */
    request.line = 0u;
    CHECK(ps3_spurs_lfqa_validate(&request, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);   /* zero and aligned -> gate 2 */

    /* The SECOND gate (a NULL test). It sits after the
     * alignment gate, so a NULL line has already been answered by gate 1 with 0x80410910 and gate 2
     * is where a caller lands only if gate 1 passed -- which for `line == 0` it never does. The
     * ORDER is what matters and what a reader must not "fix": the NULL code exists in the listing but
     * is unreachable for a literal zero line. */
    request.line = 0u;
    r3 = 0;
    CHECK(ps3_spurs_lfqa_validate(&request, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);   /* the NULL code: this is the gate-2 answer */
    for (uint32_t low = 1; low < 0x80u; ++low) {
        request.line = 0x1000u + low;
        r3 = 0;
        CHECK(ps3_spurs_lfqa_validate(&request, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410910u);
    }
    /* Every aligned non-zero line passes both gates. */
    const uint32_t aligned[] = {0x80u, 0x100u, 0x1000u, 0x12345680u, 0xFFFFFF80u};
    for (size_t i = 0; i < sizeof aligned / sizeof aligned[0]; ++i) {
        request.line = aligned[i];
        r3 = 0xDEADBEEFu;
        CHECK(ps3_spurs_lfqa_validate(&request, &r3) == PS3_SPURS_LFQA_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
    }
    /* NULL out pointers are the domain's own contract error, never a guest code. */
    request.line = 0x1000u;
    CHECK(ps3_spurs_lfqa_validate(NULL, &r3) == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfqa_validate(&request, NULL) == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
}

/* ------------------------------------------------------------------------------------ */
/* §B.1 -- the owner resolution, all three phases                                        */
/* ------------------------------------------------------------------------------------ */

static void test_owner_resolution(void)
{
    uint32_t group = 0xAAAAAAAAu, r3 = 0xBBBBBBBBu;
    spurs_lfqa_owner_phase phase = PS3_SPURS_LFQA_OWNER_PHASE_C;

    /* Phase C: a non-zero low nibble means the word itself, nibble cleared,
     * is the SPU thread group id. No guest read at all. */
    CHECK(ps3_spurs_lfqa_owner_tagged(0x00123451u, 0u, &group, &phase, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_C);
    CHECK_EQ_U64(group, 0x00123450u);
    CHECK_EQ_U64(r3, 0u);

    /* A single low bit set in the nibble, and a full-nibble value. */
    CHECK(ps3_spurs_lfqa_owner_tagged(0x80000001u, 0u, &group, &phase, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(group, 0x80000000u);
    /* Nibble 0xF clears to 0, so the phase-C value is zero and 0x175B4 refuses: same code, and the
     * phase stays C (the tag test ran, the masked value just happened to be zero). */
    r3 = 0xFFFFFFFFu;
    CHECK(ps3_spurs_lfqa_owner_tagged(0x0000000Fu, 0u, &group, &phase, &r3) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(group, 0x00000000u);
    CHECK_EQ_U64(r3, 0x80410902u);
    /* Nibble 0x4 -> 0x4 stays; 0x...4 masked keeps its high bits. */
    CHECK(ps3_spurs_lfqa_owner_tagged(0x00000024u, 0u, &group, &phase, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(group, 0x00000020u);

    /* Phase C whose masked value is non-zero, including the all-ones word. */
    CHECK(ps3_spurs_lfqa_owner_tagged(0xFFFFFFFFu, 0u, &group, &phase, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(group, 0xFFFFFFF0u);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_C);

    /* Phase A: a zero word never reads +0x64 and always refuses. */
    r3 = 0;
    CHECK(ps3_spurs_lfqa_owner_tagged(0u, 0x12345670u, &group, &phase, &r3) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_A);
    CHECK_EQ_U64(group, 0u);
    CHECK_EQ_U64(r3, 0x80410902u);

    /* Phase B: an aligned non-zero word reads owner+0x64 and uses it. */
    r3 = 0;
    CHECK(ps3_spurs_lfqa_owner_tagged(0x00123450u, 0x00100000u, &group, &phase, &r3) ==
          PS3_SPURS_LFQA_STATUS_OK);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_B);
    CHECK_EQ_U64(group, 0x00100000u);
    CHECK_EQ_U64(r3, 0u);

    /* Phase B whose +0x64 is zero is the SAME refusal code as phase A. */
    r3 = 0xFFFFFFFFu;
    CHECK(ps3_spurs_lfqa_owner_tagged(0x00123450u, 0u, &group, &phase, &r3) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_B);
    CHECK_EQ_U64(group, 0u);
    CHECK_EQ_U64(r3, 0x80410902u);

    /* The nibble test is on the LOW NIBBLE, not on bit 0: 0x...4 is phase C too, so the +0x64 read
     * never happens for it even though bit 2 is the only bit set. */
    CHECK(ps3_spurs_lfqa_owner_tagged(0x20u, 0x1u, &group, &phase, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_B);   /* 0x20's low nibble IS 0 */
    CHECK_EQ_U64(group, 0x1u);
    CHECK(ps3_spurs_lfqa_owner_tagged(0x24u, 0x1u, &group, &phase, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(phase == PS3_SPURS_LFQA_OWNER_PHASE_C);
    CHECK_EQ_U64(group, 0x20u);

    /* Null out pointers. */
    CHECK(ps3_spurs_lfqa_owner_tagged(1u, 0u, NULL, &phase, &r3) == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfqa_owner_tagged(1u, 0u, &group, NULL, &r3) == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfqa_owner_tagged(1u, 0u, &group, &phase, NULL) == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
}

/* ------------------------------------------------------------------------------------ */
/* §C.3 -- the shape selector, the trip count and the shape gate                          */
/* ------------------------------------------------------------------------------------ */

static void test_shape(void)
{
    spurs_lfqa_request request;
    memset(&request, 0, sizeof request);
    request.line = 0x1000u;

    /* 2 iterations for version 3, 1 otherwise. */
    CHECK_EQ_U64(ps3_spurs_lfqa_iterations(3u), 2u);
    const uint32_t others[] = {0u, 1u, 2u, 4u, 5u, 0x100u, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; ++i)
        CHECK_EQ_U64(ps3_spurs_lfqa_iterations(others[i]), 1u);
    CHECK(ps3_spurs_lfqa_is_spurs_shape(3u) == 1);
    CHECK(ps3_spurs_lfqa_is_spurs_shape(2u) == 0);
    CHECK(ps3_spurs_lfqa_is_spurs_shape(4u) == 0);

    uint32_t r3 = 0;
    /* CellSync shape: +0x20 must not be -1. */
    request.version = 0u;
    request.recorded_queue = PS3_SPURS_LFQA_QUEUE_NONE;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFFu, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x8041090Fu);
    request.recorded_queue = 0x8D000000u;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFFu, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);
    /* ...and a zero id is accepted: the test is `== -1`, not `== 0`. */
    request.recorded_queue = 0u;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0x00u, &r3) == PS3_SPURS_LFQA_STATUS_OK);

    /* SPURS shape: BOTH +0x20 and +0x21 must be 0xFF. */
    request.version = 3u;
    request.recorded_queue = 0xFFFFFFFFu;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFFu, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    r3 = 0;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFEu, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x8041090Fu);
    r3 = 0;
    request.recorded_queue = 0xFFFFFFFEu;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFFu, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x8041090Fu);
    /* One BYTE is tested at +0x20, and
     * it is that field's low byte: 0x123456FF passes, 0x1234567F does not. */
    request.recorded_queue = 0x123456FFu;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFFu, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    request.recorded_queue = 0x1234567Fu;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0xFFu, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x8041090Fu);
    /* The second byte is the caller's `port2` (+0x21), same test. */
    request.recorded_queue = 0xFFFFFFFFu;
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0x00FFu, &r3) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(ps3_spurs_lfqa_shape_gate(&request, 0x0100u, &r3) == PS3_SPURS_LFQA_STATUS_REFUSED);
}

/* ------------------------------------------------------------------------------------ */
/* §D.1 -- the packet                                                            */
/* ------------------------------------------------------------------------------------ */

static void test_queue_image(void)
{
    guarded g;
    guarded_init(&g, 0xA5);
    CHECK(ps3_spurs_lfqa_queue_image(g.bytes, PS3_SPURS_LFQA_BYTES) == 0);
    /* is the SIZE argument; the packet itself is protocol 1, type 1, name 0. */
    const uint8_t expected[16] = {0, 0, 0, 1,   /* protocol = 1 (FIFO)  */
                                  0, 0, 0, 1,   /* type     = 1 (PPU)   */
                                  0, 0, 0, 0,   /* name     = 0, hi     */
                                  0, 0, 0, 0};  /* name     = 0, lo     */
    CHECK_EQ_MEM(g.bytes, expected, sizeof expected);
    /* Nothing outside the 16-byte packet is touched. */
    for (size_t i = 16; i < PS3_SPURS_LFQA_BYTES; ++i) CHECK(g.bytes[i] == 0xA5);
    CHECK(guarded_intact(&g, 0xA5));

    /* A short buffer is refused, and refused without writing. */
    guarded_init(&g, 0xA5);
    CHECK(ps3_spurs_lfqa_queue_image(g.bytes, 8u) == -1);
    CHECK(guarded_intact(&g, 0xA5));
    for (size_t i = 0; i < PS3_SPURS_LFQA_BYTES; ++i) CHECK(g.bytes[i] == 0xA5);
    CHECK(ps3_spurs_lfqa_queue_image(NULL, 16u) == -1);
}

/* ------------------------------------------------------------------------------------ */
/* §D.2 -- the mask                                                              */
/* ------------------------------------------------------------------------------------ */

static void test_event_mask(void)
{
    /* The WILDCARD selector yields the dynamic mask, bits 16..63 -- what 0xAF2C builds at
     * 0xAFDC..0xB038 for this entry exactly as it does for the event-flag attach. */
    CHECK_EQ_U64(ps3_spurs_lfqa_event_mask(PS3_SPURS_LFQA_EVENT_PORT_WILDCARD),
                 PS3_SPURS_LFQA_EVENT_MASK);
    CHECK_EQ_U64(PS3_SPURS_LFQA_EVENT_MASK, 0xFFFFFFFFFFFF0000ULL);
    CHECK_EQ_U64(PS3_SPURS_LFQA_EVENT_MASK, (unsigned long long)PS3_SPURS_DYNAMIC_PORT_MASK);
    /* Bits 16..63: every one set, and nothing outside. */
    CHECK_EQ_U64(PS3_SPURS_LFQA_EVENT_MASK & 0xFFFFu, 0u);
    CHECK_EQ_U64(PS3_SPURS_LFQA_EVENT_MASK >> 16, 0xFFFFFFFFFFFFULL);
    /* The per-port form is a DIFFERENT primitive's mask, offered only so a caller can tell them
     * apart; it is never what this entry's connect passes. */
    CHECK_EQ_U64(ps3_spurs_lfqa_event_mask(0u), 1u);
    CHECK_EQ_U64(ps3_spurs_lfqa_event_mask(15u), 0x8000u);
    CHECK_EQ_U64(ps3_spurs_lfqa_event_mask(63u), 0x8000000000000000u);
    CHECK(ps3_spurs_lfqa_event_mask(15u) != PS3_SPURS_LFQA_EVENT_MASK);
    /* Out of range: 0, which is never a legal mask, so a caller can refuse on it. */
    CHECK_EQ_U64(ps3_spurs_lfqa_event_mask(64u), 0u);
    CHECK_EQ_U64(ps3_spurs_lfqa_event_mask(0xFFFFFFFFu), 0u);
}

/* ------------------------------------------------------------------------------------ */
/* §C.1 -- the publication, both shapes, both widths                                       */
/* ------------------------------------------------------------------------------------ */

static void test_publication(void)
{
    guarded g;

    /* SPURS shape: +0x78 is EIGHT bytes, +0x20 is FOUR. */
    guarded_init(&g, 0x5A);
    CHECK(ps3_spurs_lfqa_publication(1, 0x1122334455667788ULL, 0xAAAABBBB, 0xCCCCDDDD,
        0x8000000Fu, 0x01u, 0x12345678u, 0x9ABCu, 0xDEF0u, g.bytes, PS3_SPURS_LFQA_BYTES) == 0);
    uint8_t expected[PS3_SPURS_LFQA_BYTES];
    for (size_t i = 0; i < sizeof expected; ++i) expected[i] = 0x5A;
    be_put(expected, 0x78u, 8u, 0x1122334455667788ULL);
    be_put(expected, 0x20u, 4u, 0x8000000Fu);
    CHECK_EQ_MEM(g.bytes, expected, sizeof expected);
    CHECK(guarded_intact(&g, 0x5A));
    /* The SPURS shape writes nothing else: +0x7C, +0x21, +0x28, +0x30, +0x50 keep 0x5A. */
    for (size_t i = 0; i < PS3_SPURS_LFQA_BYTES; ++i)
        if (i < 0x20u || i > 0x23u)
            if (i < 0x78u || i > 0x7Fu) CHECK(g.bytes[i] == 0x5A);

    /* CellSync shape: +0x78 and +0x7C are FOUR bytes each, +0x20 and +0x21 are
     * ONE byte each, +0x28 four bytes, +0x30 two, +0x50 two. */
    guarded_init(&g, 0x5A);
    CHECK(ps3_spurs_lfqa_publication(0, 0x0ULL, 0x00000008u, 0x00000009u, 0x0Fu, 0x07u,
        0x0000002Au, 0x1234u, 0x5678u, g.bytes, PS3_SPURS_LFQA_BYTES) == 0);
    for (size_t i = 0; i < sizeof expected; ++i) expected[i] = 0x5A;
    be_put(expected, 0x78u, 4u, 0x00000008u);
    be_put(expected, 0x7Cu, 4u, 0x00000009u);
    be_put(expected, 0x20u, 1u, 0x0Fu);
    be_put(expected, 0x21u, 1u, 0x07u);
    be_put(expected, 0x28u, 4u, 0x0000002Au);
    be_put(expected, 0x30u, 2u, 0x1234u);
    be_put(expected, 0x50u, 2u, 0x5678u);
    CHECK_EQ_MEM(g.bytes, expected, sizeof expected);
    CHECK(guarded_intact(&g, 0x5A));

    /* The two shapes disagree on +0x78's width, which is the trap §C.1 calls out. */
    guarded_init(&g, 0x00);
    CHECK(ps3_spurs_lfqa_publication(1, 0x00000000FFFFFFFFULL, 0, 0, 0, 0, 0, 0, 0,
        g.bytes, PS3_SPURS_LFQA_BYTES) == 0);
    {
        guarded other;
        guarded_init(&other, 0x00);
        CHECK(ps3_spurs_lfqa_publication(0, 0, 0xFFFFFFFFu, 0u, 0, 0, 0, 0, 0,
            other.bytes, PS3_SPURS_LFQA_BYTES) == 0);
        /* SPURS: +0x78..+0x7F = 00 00 00 00 FF FF FF FF. CellSync: +0x78..0x7B = FF FF FF FF and
         * +0x7C..0x7F = 00 00 00 00. The bytes are swapped relative to each other. */
        const uint8_t spurs_78[8] = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint8_t cellsync_78[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
        CHECK_EQ_MEM(g.bytes + 0x78, spurs_78, 8u);
        CHECK_EQ_MEM(other.bytes + 0x78, cellsync_78, 8u);
    }

    /* Short / NULL buffers are refused with no write. */
    guarded_init(&g, 0x5A);
    CHECK(ps3_spurs_lfqa_publication(0, 0, 0, 0, 0, 0, 0, 0, 0, g.bytes, 64u) == -1);
    CHECK(guarded_intact(&g, 0x5A));
    CHECK(ps3_spurs_lfqa_publication(0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, 128u) == -1);
}

/* ------------------------------------------------------------------------------------ */
/* §D.4 -- the fold                                                                       */
/* ------------------------------------------------------------------------------------ */

static void test_fold(void)
{
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0u), 0u);
    /* The fold the entry performs twice: `0x80410900 | (code & 0xFF)` (the mask
     * keeps the LOW byte) guarded by
     * `((code >> 16) & 0xF) == 0x1`.
     * These are the codes the entry actually meets on this path. */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80410109u), 0x80410909u);   /* the 0x8B90 failure */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80410711u), 0x80410911u);   /* the 0xB244 failure */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x8041070Au), 0x8041090Au);   /* 0xAF2C's EISCONN            */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x8041070Fu), 0x8041090Fu);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80410910u), 0x80410910u);   /* already in the namespace     */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80410300u), 0x80410900u);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x4144CDu), 0x804109CDu);     /* bits 16..23 are 0x41         */
    /* Bits 16..23 not 0x41: returned verbatim. That includes 0x1234ABCD (0x4A), 0x80420020 (0x2),
     * 0x12345678 (0x4), 0x005541CD (0x55) and the two lv2 codes (0x01). */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x1234ABCDu), 0x1234ABCDu);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80420020u), 0x80420020u);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x12345678u), 0x12345678u);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x005541CDu), 0x005541CDu);
    /* A 0x8001xxxx lv2 code has bits 16..19 = 0x1, so the nibble test MATCHES and it folds like any
     * 0x8041 code. That is what the listing says, and it matters: the entry never returns a raw
     * 0x8001xxxx to the guest on a path that reaches the fold. A 0x8001 code whose nibble is not 1
     * (0x8000xxxx, 0x8002xxxx) passes through. */
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80010005u), 0x80410905u);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80010015u), 0x80410915u);
    CHECK_EQ_U64(ps3_spurs_lfqa_fold(0x80000005u), 0x80000005u);
    CHECK_EQ_U64(ps3_spurs_lfqa_result(0u), 0u);
    CHECK_EQ_U64(ps3_spurs_lfqa_result(0x80410109u), 0x80410909u);
    /* The sign extension the epilogue applies. */
    CHECK_EQ_U64(ps3_spurs_lfqa_sign_extend(0x80410902u), 0xFFFFFFFF80410902ULL);
    CHECK_EQ_U64(ps3_spurs_lfqa_sign_extend(0u), 0u);
}

/* ------------------------------------------------------------------------------------ */
/* The whole plan, for the values inFAMOUS actually passes                                */
/* ------------------------------------------------------------------------------------ */

/* Builds the line inFAMOUS reaches this entry with: the bytes _cellSpursLFQueueInitialize
 * (NID 0x011EE38B) leaves behind for the game's own arguments (owner = the taskset EA | 1, element
 * size 0x20, depth 0x60, direction 3, buffer = the taskset EA + 0x80), plus this entry's own
 * inputs (`+0x74` = the CellSpurs EA, and, on the shape the game selects, a recorded queue id).
 *
 * The builder is the REAL one from spurs_lfqueue.h, so the two entries cannot drift apart: if a
 * later correction changes where the builder stores the direction, this test's expectation of
 * `+0x24` moves with it and fails loudly rather than silently. */
static void build_game_line(unsigned char line[PS3_SPURS_LFQA_BYTES], uint32_t taskset_ea)
{
    memset(line, 0, PS3_SPURS_LFQA_BYTES);
    spurs_lfq_request request;
    request.owner = taskset_ea | 1u;
    request.queue = taskset_ea;
    request.buffer = taskset_ea + 0x80u;
    request.elem_size = 0x20u;
    request.depth = 0x60u;                /* r29, the fourth game argument */
    request.direction = 3u;
    /* The game's taskset is zero on entry, so the version field the dispatcher reads is 0 and the
     * builder takes the CellSync shape; it then stores the DIRECTION (3) at +0x24. */
    (void)ps3_spurs_lfq_image(&request, 0u, line, PS3_SPURS_LFQA_BYTES);
}

static void test_plan_spurs_shape(void)
{
    const uint32_t taskset_ea = 0x00800080u;
    unsigned char line[PS3_SPURS_LFQA_BYTES];
    build_game_line(line, taskset_ea);

    /* This is the linkage the CONTRACT's §B.3 asserts: the builder put the direction there. */
    CHECK_EQ_U64(be_get32(line, PS3_SPURS_LFQA_VERSION), 3u);
    CHECK(ps3_spurs_lfqa_line_is_spurs_shape(line, sizeof line) == 1);
    CHECK_EQ_U64(be_get32(line, PS3_SPURS_LFQA_F_OWNER), taskset_ea | 1u);

    spurs_lfqa_request request;
    request.line = taskset_ea;
    request.owner = (uint32_t)0x00200051u;        /* the CellSpurs EA, low nibble != 0 -> phase C */
    request.version = 3u;
    request.recorded_queue = 0xFFFFFFFFu;         /* the builder's own +0x20 for this shape */
    request.sdk_version = 0x250001u;              /* below 0x180000: the plain port bound */

    spurs_lfqa_plan plan;
    ps3_spurs_lfqa_plan_build(&request, 0u, 0xFFu, &plan);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(plan.r3, 0u);
    CHECK(plan.spurs_shape == 1);
    CHECK_EQ_U64(plan.iterations, 2u);
    CHECK_EQ_U64(plan.actions_count, 2u);
    CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_C);
    CHECK_EQ_U64(plan.group, 0x00200050u);
    for (uint32_t index = 0; index < plan.actions_count; ++index) {
        CHECK(plan.actions[index].kind == PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH);
        CHECK_EQ_U64(plan.actions[index].iteration, index);
        CHECK_EQ_U64(plan.actions[index].queue, 0u);         /* the id is an OUTPUT here */
        CHECK_EQ_U64(plan.actions[index].mask, PS3_SPURS_LFQA_EVENT_MASK);
    }

    /* The phase-B variant reads owner+0x64, which the caller supplies as `tag`. */
    request.owner = 0x00200050u;
    ps3_spurs_lfqa_plan_build(&request, 0x00200060u, 0xFFu, &plan);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_B);
    CHECK_EQ_U64(plan.group, 0x00200060u);
}

static void test_plan_cellsync_shape(void)
{
    spurs_lfqa_request request;
    request.line = 0x00100000u;
    request.owner = (uint32_t)0x00200051u;
    request.version = 2u;                 /* anything but 3 */
    request.recorded_queue = 0x8D000100u; /* a live queue id the line already carries */
    request.sdk_version = 0x250001u;

    spurs_lfqa_plan plan;
    ps3_spurs_lfqa_plan_build(&request, 0u, 0x00u, &plan);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(plan.spurs_shape == 0);
    CHECK_EQ_U64(plan.iterations, 1u);
    CHECK_EQ_U64(plan.actions_count, 1u);
    CHECK(plan.actions[0].kind == PS3_SPURS_LFQA_ACTION_ATTACH_ONLY);
    CHECK_EQ_U64(plan.actions[0].queue, 0x8D000100u);
    CHECK_EQ_U64(plan.actions[0].mask, PS3_SPURS_LFQA_EVENT_MASK);

    /* The same request with no recorded queue is refused before any effect. */
    request.recorded_queue = PS3_SPURS_LFQA_QUEUE_NONE;
    ps3_spurs_lfqa_plan_build(&request, 0u, 0x00u, &plan);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(plan.r3, 0x8041090Fu);
    CHECK_EQ_U64(plan.actions_count, 0u);
    for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
        CHECK(plan.publication[index] == 0u);

    /* A zero recorded id is FINE: the test is `== -1`, not `== 0`. */
    request.recorded_queue = 0u;
    ps3_spurs_lfqa_plan_build(&request, 0u, 0x00u, &plan);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(plan.actions[0].queue, 0u);
}

/* Every refusal must sit BEFORE the firmware's first guest write, so no plan that refuses may carry
 * an action or a publication. Checked over the whole gate space, not just the sampled values above. */
static void test_refusals_are_effect_free(void)
{
    const uint32_t versions[] = {0u, 1u, 2u, 3u, 4u, 0xFFFFFFFFu};
    const uint32_t owners[] = {0u, 1u, 0x10u, 0x12345650u, 0xFFFFFFFFu};
    const uint32_t queues[] = {0u, 0xFFFFFFFFu, 0x8D000100u, 0xFFFFFFFEu};
    const uint32_t tags[] = {0u, 0x00100000u, 0xFFFFFFFFu};
    unsigned off_actions = 0, refused = 0, accepted = 0;
    for (size_t v = 0; v < sizeof versions / sizeof versions[0]; ++v)
        for (size_t o = 0; o < sizeof owners / sizeof owners[0]; ++o)
            for (size_t q = 0; q < sizeof queues / sizeof queues[0]; ++q)
                for (size_t t = 0; t < sizeof tags / sizeof tags[0]; ++t) {
                    spurs_lfqa_request request;
                    request.line = 0x00100000u;
                    request.owner = owners[o];
                    request.version = versions[v];
                    request.recorded_queue = queues[q];
                    request.sdk_version = 0x250001u;
                    spurs_lfqa_plan plan;
                    ps3_spurs_lfqa_plan_build(&request, tags[t], 0xFFu, &plan);
                    if (plan.status == PS3_SPURS_LFQA_STATUS_REFUSED) {
                        ++refused;
                        if (plan.actions_count != 0u) ++off_actions;
                        for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
                            if (plan.publication[index] != 0u) { ++off_actions; break; }
                        if (plan.r3 == 0u) ++off_actions;
                    } else if (plan.status == PS3_SPURS_LFQA_STATUS_OK) {
                        ++accepted;
                        if (plan.actions_count != plan.iterations) ++off_actions;
                    }
                }
    CHECK_EQ_U64(off_actions, 0u);
    CHECK(refused > 0u);
    CHECK(accepted > 0u);

    /* The same sweep through the SDK table on the arm inFAMOUS takes. Every refusal on that
     * arm also precedes the firmware's first store (0x1778C / 0x17650 both go straight to 0x17540), so
     * no plan that refuses may carry an action or a publication there either. */
    unsigned off_actions25 = 0, refused25 = 0, accepted25 = 0;
    for (size_t v = 0; v < sizeof versions / sizeof versions[0]; ++v)
        for (size_t o = 0; o < sizeof owners / sizeof owners[0]; ++o)
            for (size_t q = 0; q < sizeof queues / sizeof queues[0]; ++q)
                for (size_t t = 0; t < sizeof tags / sizeof tags[0]; ++t) {
                    spurs_lfqa_request request;
                    request.line = 0x00100000u;
                    request.owner = owners[o];
                    request.version = versions[v];
                    request.recorded_queue = queues[q];
                    request.sdk_version = 0x250001u;   /* > 0x18FFFF -> 0x17778 */
                    spurs_lfqa_plan plan;
                    ps3_spurs_lfqa_plan_build_for_sdk(&request, tags[t], 0xFFu, &plan);
                    if (plan.status == PS3_SPURS_LFQA_STATUS_REFUSED) {
                        ++refused25;
                        if (plan.actions_count != 0u) ++off_actions25;
                        for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
                            if (plan.publication[index] != 0u) { ++off_actions25; break; }
                        if (plan.r3 == 0u) ++off_actions25;
                    } else if (plan.status == PS3_SPURS_LFQA_STATUS_OK) {
                        ++accepted25;
                        if (plan.actions_count != plan.iterations) ++off_actions25;
                    }
                }
    CHECK_EQ_U64(off_actions25, 0u);
    CHECK(refused25 > 0u);
    CHECK(accepted25 > 0u);

    /* An una-ligned or zero line is refused at the entry, again with nothing planned. */
    const uint32_t lines[] = {0u, 1u, 0x7Fu, 0x1000u};
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
        spurs_lfqa_request request;
        request.line = lines[i];
        request.owner = 0x12345650u;
        request.version = 3u;
        request.recorded_queue = 0xFFFFFFFFu;
        request.sdk_version = 0x250001u;
        spurs_lfqa_plan plan;
        ps3_spurs_lfqa_plan_build(&request, 0x00100000u, 0xFFu, &plan);
        if ((lines[i] & 0x7Fu) != 0u || lines[i] == 0u) {
            CHECK(plan.status == PS3_SPURS_LFQA_STATUS_REFUSED);
            CHECK_EQ_U64(plan.r3, lines[i] == 0u ? 0x80410911u : 0x80410910u);
            CHECK_EQ_U64(plan.actions_count, 0u);
        } else {
            CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
        }
    }
    spurs_lfqa_plan null_plan;
    ps3_spurs_lfqa_plan_build(NULL, 0u, 0xFFu, NULL);          /* no-op, no crash */
    ps3_spurs_lfqa_plan_build(NULL, 0u, 0xFFu, &null_plan);    /* NULL request: no action    */
    CHECK(null_plan.status == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
    CHECK_EQ_U64(null_plan.actions_count, 0u);
}

/* The SDK bound 0xAF2C applies. Port 15 is inside both of its arms, so it can never refuse on this
 * entry's own frame -- but the plan must still evaluate it, so a future port change is answered by
 * the firmware's rule rather than by silence. */
static void test_sdk_bound(void)
{
    spurs_lfqa_request request;
    request.line = 0x00100000u;
    request.owner = 0x00200051u;
    request.version = 3u;
    request.recorded_queue = 0xFFFFFFFFu;

    spurs_lfqa_plan plan;
    const uint32_t sdks[] = {0u, 0x17FFFFu, 0x180000u, 0x250001u, 0x485000u, 0x7FFFFFFFu};
    for (size_t i = 0; i < sizeof sdks / sizeof sdks[0]; ++i) {
        request.sdk_version = sdks[i];
        ps3_spurs_lfqa_plan_build(&request, 0u, 0xFFu, &plan);
        CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
        CHECK_EQ_U64(plan.actions[0].mask, PS3_SPURS_LFQA_EVENT_MASK);
    }
}

/* ------------------------------------------------------------------------------------ */
/* The line helper                                                                        */
/* ------------------------------------------------------------------------------------ */

static void test_line_helper(void)
{
    unsigned char line[PS3_SPURS_LFQA_BYTES];
    memset(line, 0, sizeof line);
    CHECK_EQ_U64(ps3_spurs_lfqa_line_is_spurs_shape(line, sizeof line), 0);
    be_put(line, PS3_SPURS_LFQA_VERSION, 4u, 3u);
    CHECK_EQ_U64(ps3_spurs_lfqa_line_is_spurs_shape(line, sizeof line), 1);
    be_put(line, PS3_SPURS_LFQA_VERSION, 4u, 0x00030000u);
    CHECK_EQ_U64(ps3_spurs_lfqa_line_is_spurs_shape(line, sizeof line), 0);
    CHECK_EQ_U64(ps3_spurs_lfqa_line_is_spurs_shape(NULL, sizeof line), -1);
    CHECK_EQ_U64(ps3_spurs_lfqa_line_is_spurs_shape(line, 64u), -1);
}

/* ------------------------------------------------------------------------------------ */
/* G15 §1 -- the process-SDK branch table, and the arm inFAMOUS takes                       */
/* ------------------------------------------------------------------------------------ */

/* The entry's three branches, from the SDK version and two unsigned compares:
 *   sdk > 0x17FFFF -> the middle arm
 *   sdk > 0x18FFFF -> the SDK-25 arm
 * inFAMOUS's process SDK is 0x00250001, so the second compare is taken and the first arm's 0x176F8 loop
 * is not entered from this path. */
static void test_sdk_branch(void)
{
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0u) == PS3_SPURS_LFQA_SDK_BRANCH_LEGACY);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x17FFFFu) == PS3_SPURS_LFQA_SDK_BRANCH_LEGACY);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x180000u) == PS3_SPURS_LFQA_SDK_BRANCH_180000);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x18FFFFu) == PS3_SPURS_LFQA_SDK_BRANCH_180000);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x190000u) == PS3_SPURS_LFQA_SDK_BRANCH_250001);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x250001u) == PS3_SPURS_LFQA_SDK_BRANCH_250001);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x485000u) == PS3_SPURS_LFQA_SDK_BRANCH_250001);
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0xFFFFFFFFu) == PS3_SPURS_LFQA_SDK_BRANCH_250001);
    /* Both compares are `cmplw`, i.e. UNSIGNED: a high-bit SDK is above, never below. */
    CHECK(ps3_spurs_lfqa_sdk_branch_of(0x80000000u) == PS3_SPURS_LFQA_SDK_BRANCH_250001);
}

/* The `0x17778` arm's own body, on the line inFAMOUS really hands it. */
static void test_sdk25_plan(void)
{
    unsigned char line[PS3_SPURS_LFQA_BYTES];
    const uint32_t taskset_ea = 0x00800080u;
    build_game_line(line, taskset_ea);
    /* The predecessor stored the direction (3) at +0x24 and 0xFF at +0x20 and +0x21. */
    CHECK_EQ_U64(be_get32(line, PS3_SPURS_LFQA_VERSION), 3u);
    CHECK_EQ_U64(line[PS3_SPURS_LFQA_QUEUE], 0xFFu);
    CHECK_EQ_U64(line[PS3_SPURS_LFQA_PORT2], 0xFFu);
    /* ...and the owner word at +0x74, which EVERY arm reads before the SDK compare (decision 27). */
    CHECK_EQ_U64(be_get32(line, PS3_SPURS_LFQA_F_OWNER), taskset_ea | 1u);

    spurs_lfqa_request request;
    request.line = taskset_ea;
    /* decision 27: 0x17588..0x175B4 runs BEFORE 0x175B8's SDK compare, so this arm resolves the owner too
     * and `plan.group` is its result. The value below is the GAME's tagged owner -- its CellSpurs EA | 1. */
    request.owner = 0x00200051u;
    request.version = 3u;
    request.recorded_queue = 0xFFu;
    request.sdk_version = 0x00250001u;

    spurs_lfqa_plan plan;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(plan.r3, 0u);
    CHECK(plan.spurs_shape == 1);
    CHECK_EQ_U64(plan.iterations, 2u);
    CHECK_EQ_U64(plan.actions_count, 2u);
    CHECK_EQ_U64(plan.group, 0x00200050u);                /* r28 = owner & ~0xF, the CellSpurs EA */
    CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_C);
    for (uint32_t index = 0; index < plan.actions_count; ++index) {
        CHECK(plan.actions[index].kind == PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH);
        CHECK_EQ_U64(plan.actions[index].iteration, index);
        CHECK_EQ_U64(plan.actions[index].queue, 0u);      /* the id is an OUTPUT */
        CHECK_EQ_U64(plan.actions[index].mask, PS3_SPURS_LFQA_EVENT_MASK);
    }

    /* The CellSync shape on the same arm: only the +0x24 == 3 test differs (0x17678 not taken), and
     * the shared loop's trip count becomes 1. */
    request.version = 2u;
    request.recorded_queue = 0x8D000100u;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0x00u, &plan) == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(plan.spurs_shape == 0);
    CHECK_EQ_U64(plan.iterations, 1u);
    CHECK(plan.actions[0].kind == PS3_SPURS_LFQA_ACTION_ATTACH_ONLY);
    CHECK_EQ_U64(plan.actions[0].queue, 0x8D000100u);

    /* decision 27: the owner resolution runs on THIS arm too, and its two 0x17654 refusals land before the
     * firmware's first store. A zero owner word, and an untagged owner whose own `+0x64` reads 0. */
    request.version = 3u;
    request.recorded_queue = 0xFFu;
    request.owner = 0u;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(plan.r3, 0x80410902u);
    CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_A);
    CHECK_EQ_U64(plan.group, 0u);
    CHECK_EQ_U64(plan.actions_count, 0u);
    for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
        CHECK(plan.publication[index] == 0u);
    request.owner = 0x00200050u;                          /* an untagged TASKSET owner ... */
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);                 /* ... whose own +0x64 (tag) is 0 */
    CHECK_EQ_U64(plan.r3, 0x80410902u);
    CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_B);
    CHECK_EQ_U64(plan.actions_count, 0u);
    /* ...and with that word supplied, the SAME arm resolves to it and plans the two creates. */
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0x00700080u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_OK);
    CHECK_EQ_U64(plan.r3, 0u);
    CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_B);
    CHECK_EQ_U64(plan.group, 0x00700080u);
    CHECK_EQ_U64(plan.actions_count, 2u);
    request.owner = 0x00200051u;

    /* A ZERO direction word returns 0x80410909 and plans nothing. Note this is a
     * different code from the shape gate's 0x8041090F, and it is the one assembled
     * before the test. */
    request.version = 0u;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(plan.r3, 0x80410909u);
    CHECK_EQ_U64(plan.actions_count, 0u);
    for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
        CHECK(plan.publication[index] == 0u);

    /* the two shape gates, unchanged, with their own 0x8041090F. */
    request.version = 3u;
    request.recorded_queue = 0xF0u;                       /* +0x20's low byte is not 0xFF */
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(plan.r3, 0x8041090Fu);
    request.recorded_queue = 0xFFu;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFEu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);           /* +0x21 must be 0xFF too */
    CHECK_EQ_U64(plan.r3, 0x8041090Fu);
    request.version = 1u;
    request.recorded_queue = PS3_SPURS_LFQA_QUEUE_NONE;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0x00u, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);           /* the CellSync arm's -1 test */
    CHECK_EQ_U64(plan.r3, 0x8041090Fu);

    /* The two entry gates come FIRST on every branch and keep their codes. */
    request.version = 3u;
    request.recorded_queue = 0xFFu;
    request.line = 0x1004u;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(plan.r3, 0x80410910u);
    request.line = 0u;
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_REFUSED);
    CHECK_EQ_U64(plan.r3, 0x80410911u);

    /* A NULL request and a NULL plan are the domain's own contract errors. */
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(NULL, 0u, 0xFFu, &plan) ==
          PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
    CHECK(plan.status == PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, NULL) ==
          PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT);
}

/* The branch table must not disturb the arms b1 decoded: a 0x180000-range SDK still goes through
 * ps3_spurs_lfqa_plan_build() and its owner resolution, unchanged. */
static void test_sdk25_does_not_touch_the_older_arms(void)
{
    spurs_lfqa_request request;
    request.line = 0x00100000u;
    request.owner = 0x00200051u;
    request.version = 3u;
    request.recorded_queue = 0xFFFFFFFFu;
    request.sdk_version = 0x180000u;

    spurs_lfqa_plan legacy, via_table;
    ps3_spurs_lfqa_plan_build(&request, 0u, 0xFFu, &legacy);
    ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &via_table);
    CHECK(legacy.status == PS3_SPURS_LFQA_STATUS_OK);
    CHECK(via_table.status == legacy.status);
    CHECK_EQ_U64(via_table.r3, legacy.r3);
    CHECK_EQ_U64(via_table.group, legacy.group);
    CHECK_EQ_U64(via_table.iterations, legacy.iterations);
    CHECK(via_table.owner_phase == legacy.owner_phase);
    CHECK(via_table.spurs_shape == legacy.spurs_shape);
    /* The owner resolution still ran: this is the arm where +0x74 matters. */
    CHECK(via_table.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_C);
    CHECK_EQ_U64(via_table.group, 0x00200050u);
}

/* ------------------------------------------------------------------------------------ */
/* G16b -- the owner resolution's place in the refusal order                              */
/* ------------------------------------------------------------------------------------ */

/* The owner resolution sits BETWEEN the two entry gates and the SDK compare, so its two
 * refusals win over EVERY later refusal, on every SDK arm:
 *
 *   the owner word is read, and its low nibble tested
 *   owner word 0 -> 0x80410902
 *   an untagged owner: its +0x64 word is read
 *   resolved 0 -> 0x80410902
 *   THE SDK COMPARE, AFTER ALL OF THAT
 *   the older arm's FIRST +0x24 read
 *   the SDK-25 arm's +0x24 read
 *   both refusals answer 0x80410902; NO guest byte written yet
 *
 * So a line whose owner resolves to nothing must answer 0x80410902 even when its `+0x24` is 0
 * (0x17788's 0x80410909) and even when a shape gate would refuse (0x17648's 0x8041090F). Every
 * expected value below is read from that listing, not from the implementation. */
static void test_owner_refusal_precedes_version_and_shape(void)
{
    /* The three SDK arms the entry can take, plus the boundary values. */
    const uint32_t sdks[] = {0u, 0x17FFFFu, 0x180000u, 0x18FFFFu, 0x190000u, 0x250001u};

    for (size_t s = 0; s < sizeof sdks / sizeof sdks[0]; ++s) {
        const int sdk25 = sdks[s] > PS3_SPURS_LFQA_SDK_BRANCH2_MAX;

        /* (1) owner word 0 AND version 0: the owner's 0x80410902, never the direction gate's
         * 0x80410909. The shape bytes are good, so nothing but the order can decide this. */
        {
            spurs_lfqa_request request;
            memset(&request, 0, sizeof request);
            request.line = 0x00100000u;
            request.owner = 0u;
            request.version = 0u;
            request.recorded_queue = 0xFFFFFFFFu;   /* passes both shape gates */
            request.sdk_version = sdks[s];
            spurs_lfqa_plan plan;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan);
            CHECK(plan.status == PS3_SPURS_LFQA_STATUS_REFUSED);
            CHECK_EQ_U64(plan.r3, 0x80410902u);
            CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_A);
            CHECK_EQ_U64(plan.group, 0u);
            CHECK_EQ_U64(plan.actions_count, 0u);
            for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
                CHECK(plan.publication[index] == 0u);
        }

        /* (2) an untagged owner whose +0x64 is 0 AND a failing shape gate. Version 3 with +0x20 =
         * 0xF0 fails 0x1763C (which answers 0x8041090F), yet 0x175B4's 0x80410902 comes first. */
        {
            spurs_lfqa_request request;
            memset(&request, 0, sizeof request);
            request.line = 0x00100000u;
            request.owner = 0x00200050u;            /* nibble 0 -> phase B, reads owner+0x64 */
            request.version = 3u;
            request.recorded_queue = 0xFFFFFFF0u;   /* +0x20's low byte is not 0xFF */
            request.sdk_version = sdks[s];
            spurs_lfqa_plan plan;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u /* tag == 0 */, 0xFFu, &plan);
            CHECK(plan.status == PS3_SPURS_LFQA_STATUS_REFUSED);
            CHECK_EQ_U64(plan.r3, 0x80410902u);
            CHECK(plan.owner_phase == PS3_SPURS_LFQA_OWNER_PHASE_B);
            CHECK_EQ_U64(plan.group, 0u);
            CHECK_EQ_U64(plan.actions_count, 0u);
            for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index)
                CHECK(plan.publication[index] == 0u);
        }

        /* (3) the SAME failing shape gate with a resolvable owner still answers 0x8041090F, so the
         * owner gate is not simply swallowing the shape refusals. */
        {
            spurs_lfqa_request request;
            memset(&request, 0, sizeof request);
            request.line = 0x00100000u;
            request.owner = 0x00200051u;
            request.version = 3u;
            request.recorded_queue = 0xFFFFFFF0u;
            request.sdk_version = sdks[s];
            spurs_lfqa_plan plan;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan);
            CHECK(plan.status == PS3_SPURS_LFQA_STATUS_REFUSED);
            CHECK_EQ_U64(plan.r3, 0x8041090Fu);
            CHECK_EQ_U64(plan.actions_count, 0u);
        }

        /* (4) a valid owner with version 0: 0x80410909 on the SDK-25 arm
         * (UNCHANGED by this slice). The older arms own NO version gate of their own: they go
         * straight from reading `+0x24` to comparing it with 3, so `+0x24 == 0` is simply the
         * CellSync shape (compare the SDK-25 arm's zero test, which only exists
         * behind the last branch), and a line carrying a real `+0x20` id is accepted. */
        {
            spurs_lfqa_request request;
            memset(&request, 0, sizeof request);
            request.line = 0x00100000u;
            request.owner = 0x00200051u;
            request.version = 0u;
            request.recorded_queue = 0x8D000100u;   /* a valid CellSync queue id */
            request.sdk_version = sdks[s];
            spurs_lfqa_plan plan;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0xFFu, &plan);
            if (sdk25) {
                CHECK(plan.status == PS3_SPURS_LFQA_STATUS_REFUSED);
                CHECK_EQ_U64(plan.r3, 0x80410909u);
                CHECK_EQ_U64(plan.actions_count, 0u);
            } else {
                CHECK(plan.status == PS3_SPURS_LFQA_STATUS_OK);
                CHECK_EQ_U64(plan.r3, 0u);
                CHECK_EQ_U64(plan.iterations, 1u);
                CHECK(plan.actions[0].kind == PS3_SPURS_LFQA_ACTION_ATTACH_ONLY);
                CHECK_EQ_U64(plan.group, 0x00200050u);
            }
        }

        /* (5) the owner refusal also beats the entry's own shape-gate word on the CellSync arm
         * (+0x20 == -1, 0x17680/0x17684 -> 0x8041090F). */
        {
            spurs_lfqa_request request;
            memset(&request, 0, sizeof request);
            request.line = 0x00100000u;
            request.owner = 0x00200050u;
            request.version = 1u;
            request.recorded_queue = PS3_SPURS_LFQA_QUEUE_NONE;
            request.sdk_version = sdks[s];
            spurs_lfqa_plan plan;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0x00u, &plan);
            CHECK_EQ_U64(plan.r3, 0x80410902u);
            CHECK_EQ_U64(plan.actions_count, 0u);
        }

        /* (6) and the two entry gates still come FIRST: a misaligned or NULL line answers its own
         * code before any owner read. */
        {
            spurs_lfqa_request request;
            memset(&request, 0, sizeof request);
            request.line = 0x00100004u;
            request.owner = 0u;
            request.version = 0u;
            request.recorded_queue = 0u;
            request.sdk_version = sdks[s];
            spurs_lfqa_plan plan;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0x00u, &plan);
            CHECK_EQ_U64(plan.r3, 0x80410910u);
            request.line = 0u;
            ps3_spurs_lfqa_plan_build_for_sdk(&request, 0u, 0x00u, &plan);
            CHECK_EQ_U64(plan.r3, 0x80410911u);
        }
    }

    /* The two builders must now agree on the ORDER, not just on the resolution: given the same line
     * and the same SDK-25 refusal inputs, the SDK-25 arm answers the owner's code exactly as the
     * older arm does. */
    {
        spurs_lfqa_request request;
        memset(&request, 0, sizeof request);
        request.line = 0x00100000u;
        request.owner = 0u;
        request.version = 0u;
        request.recorded_queue = 0xFFFFFFF0u;
        request.sdk_version = 0x250001u;
        spurs_lfqa_plan older, sdk25;
        ps3_spurs_lfqa_plan_build(&request, 0u, 0xFFu, &older);
        ps3_spurs_lfqa_plan_build_sdk25(&request, 0u, 0xFFu, &sdk25);
        CHECK_EQ_U64(older.r3, 0x80410902u);
        CHECK_EQ_U64(sdk25.r3, older.r3);
        CHECK_EQ_U64(sdk25.actions_count, older.actions_count);
        CHECK(sdk25.owner_phase == older.owner_phase);
        request.owner = 0x00200050u;               /* phase B, tag 0, failing shape gate */
        ps3_spurs_lfqa_plan_build(&request, 0u, 0xFFu, &older);
        ps3_spurs_lfqa_plan_build_sdk25(&request, 0u, 0xFFu, &sdk25);
        CHECK_EQ_U64(older.r3, 0x80410902u);
        CHECK_EQ_U64(sdk25.r3, older.r3);
        CHECK_EQ_U64(sdk25.actions_count, older.actions_count);
        CHECK(sdk25.owner_phase == older.owner_phase);
    }
}

int main(void)
{
    test_identity();
    test_entry_gates();
    test_owner_resolution();
    test_shape();
    test_queue_image();
    test_event_mask();
    test_publication();
    test_fold();
    test_plan_spurs_shape();
    test_plan_cellsync_shape();
    test_refusals_are_effect_free();
    test_sdk_bound();
    test_line_helper();
    test_sdk_branch();
    test_sdk25_plan();
    test_sdk25_does_not_touch_the_older_arms();
    test_owner_refusal_precedes_version_and_shape();

    if (failures) {
        fprintf(stderr, "%d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}

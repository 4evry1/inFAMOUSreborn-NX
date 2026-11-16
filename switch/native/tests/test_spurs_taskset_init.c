/* Host tests for the platform-free SPURS struct-init unit
 * (native/domain/spurs_taskset_init.c).
 *
 * Both entries the game reaches are pure: validate the argument values, then produce one
 * big-endian guest struct image. These tests drive every validation branch, prove the exact
 * byte images for inFAMOUS's own argument values, and prove that no byte
 * outside the struct is written by canary-checking the caller's buffer.
 *
 * Expected images are written out here field by field from CONTRACT.md's offset tables, not
 * captured from the implementation.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spurs_taskset_init.h"
/* The same block's creation entry. */
#include "spurs_taskset_create.h"
/* The next entry the game reaches. */
#include "spurs_lfqueue.h"

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
/* Canary-guarded buffer                                                                */
/* ------------------------------------------------------------------------------------ */

/* The domain is handed a slice of a larger buffer. The guard bytes before and after the
 * slice must be untouched after every call, which is how "no byte outside the struct is
 * written" is proved at the domain boundary. */
#define GUARD 64u
typedef struct guarded {
    uint8_t storage[GUARD + 4096u + GUARD];
    uint8_t *image;
    size_t capacity;
} guarded;

static void guarded_init(guarded *g, size_t capacity)
{
    memset(g, 0xA5, sizeof(g->storage));
    g->image = g->storage + GUARD;
    g->capacity = capacity;
}

static int guarded_intact(const guarded *g)
{
    for (size_t i = 0; i < GUARD; ++i) {
        if (g->storage[i] != 0xA5) return 0;
        if (g->storage[GUARD + g->capacity + i] != 0xA5) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Expected images                                                                      */
/* ------------------------------------------------------------------------------------ */

static void be_put(uint8_t *out, size_t offset, size_t width, uint64_t value)
{
    for (size_t i = 0; i < width; ++i)
        out[offset + i] = (uint8_t)(value >> (8u * (width - 1u - i)));
}

static uint64_t be_get(const uint8_t *in, size_t offset, size_t width)
{
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value = value << 8 | in[offset + i];
    return value;
}

/* CellSpursTasksetAttribute as CONTRACT.md §A.4 builds it: memset 512, then
 * +0x00 revision, +0x04 sdk_version, +0x08 args, +0x10 priority[8], +0x18 max_contention,
 * +0x20 6400; +0x1C and +0x24 stay zero. */
static void expected_attribute(uint8_t out[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES],
                               uint32_t revision, uint32_t sdk_version, uint64_t args,
                               const uint8_t priority[8], uint32_t max_contention)
{
    memset(out, 0, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    be_put(out, 0x00, 4, revision);
    be_put(out, 0x04, 4, sdk_version);
    be_put(out, 0x08, 8, args);
    memcpy(out + 0x10, priority, 8);
    be_put(out, 0x18, 4, max_contention);
    be_put(out, 0x1C, 4, 0);
    be_put(out, 0x20, 4, 6400);
    be_put(out, 0x24, 4, 0);
}

/* CellSpursEventFlag as CONTRACT.md §B.3 builds it. `taskset != 0` selects the branch with
 * addr = taskset and isIwl = 0; `taskset == 0` selects addr = spurs and isIwl = 1. */
static void expected_event_flag(uint8_t out[PS3_SPURS_EVENT_FLAG_BYTES], uint32_t spurs,
                                uint32_t taskset, uint32_t clear_mode, uint32_t direction)
{
    memset(out, 0, PS3_SPURS_EVENT_FLAG_BYTES);
    be_put(out, 0x0C, 1, 0xFF);
    be_put(out, 0x0D, 1, taskset ? 0u : 1u);
    be_put(out, 0x0E, 1, direction);
    be_put(out, 0x0F, 1, clear_mode);
    be_put(out, 0x70, 8, taskset ? taskset : spurs);
}

static void check_all_zero(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) CHECK(bytes[i] == 0u);
}

/* ------------------------------------------------------------------------------------ */
/* Attribute: success and the exact inFAMOUS image                                      */
/* ------------------------------------------------------------------------------------ */

static void test_attribute_infamous_image(void)
{
    /* Survey §A.2.2: r3 = SP+0x90, r4 = 1, r5 = 0x00250001, r6 = 0, r7 = SP+0x78,
     * r8 = 8. The eight bytes at SP+0x78 are the guest priority array {8, 1} the game packs
     * into r5's immediate at its call site, per survey §A.2.2's `0x00250001` argument: the
     * contract copies whatever those eight bytes hold, so a {8,1,0,...} array and a
     * {0x25,0x00,0x01,0,...} array are both modelled here. */
    static const uint8_t priority[8] = {8, 1, 0, 0, 0, 0, 0, 0};
    guarded g;
    uint8_t expected[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint32_t r3 = 0xDEADBEEFu;
    spurs_taskset_attribute_request request = {
        0x0D00FF90u, /* SP+0x90 */
        1u,          /* revision  */
        0x00250001u, /* sdk_version */
        0u,          /* args       */
        0x0801000000000000u, /* the same eight bytes, big-endian */
        8u           /* max_contention */
    };
    guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    const spurs_taskset_init_status status = ps3_spurs_taskset_attribute_init(
        &request, g.image, g.capacity, &r3);
    expected_attribute(expected, 1u, 0x00250001u, 0u, priority, 8u);

    CHECK(status == PS3_SPURS_TASKSET_INIT_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(guarded_intact(&g));

    /* The same buffer, read through the public field offsets, must show what CONTRACT §C
     * states: taskset_size 6400 at +0x20, name and enable_clear_ls zero, the 472-byte
     * reserved tail zero. */
    CHECK(g.image[PS3_SPURS_TSATTR_TASKSET_SIZE + 0] == 0x00);
    CHECK(g.image[PS3_SPURS_TSATTR_TASKSET_SIZE + 1] == 0x00);
    CHECK(g.image[PS3_SPURS_TSATTR_TASKSET_SIZE + 2] == 0x19);
    CHECK(g.image[PS3_SPURS_TSATTR_TASKSET_SIZE + 3] == 0x00);
    check_all_zero(g.image + PS3_SPURS_TSATTR_NAME, 4);
    check_all_zero(g.image + PS3_SPURS_TSATTR_ENABLE_CLEAR_LS, 4);
    check_all_zero(g.image + 0x28, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES - 0x28);
    CHECK_EQ_MEM(g.image + PS3_SPURS_TSATTR_PRIORITY, priority, 8);
}

static void test_attribute_copies_priority_verbatim(void)
{
    /* The firmware memcpys the eight priority bytes and never interprets them, so the
     * full 0x00..0x0F range survives verbatim. */
    static const uint8_t priority[8] = {0x00, 0x01, 0x07, 0x0F, 0x0E, 0x02, 0x0D, 0x03};
    guarded g;
    uint8_t expected[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint32_t r3 = 0;
    spurs_taskset_attribute_request request = {0x2F000000u, 7u, 0x00360000u,
                                               0x0123456789ABCDEFu, 0x0001070F0E020D03u, 31u};
    guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    expected_attribute(expected, 7u, 0x00360000u, 0x0123456789ABCDEFu, priority, 31u);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(guarded_intact(&g));
}

static void test_attribute_no_revision_branch(void)
{
    /* CONTRACT §A.1: revision and sdk_version are stored verbatim and select no code path,
     * so two very different values produce the same field layout. */
    static const uint8_t priority[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    guarded a, b;
    uint8_t expected_a[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint8_t expected_b[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint32_t r3 = 0;
    spurs_taskset_attribute_request low = {0x1000u, 0u, 0x00000000u, 0u, 0u, 0u};
    spurs_taskset_attribute_request high = {0x1000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u, 0u};
    guarded_init(&a, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    guarded_init(&b, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(ps3_spurs_taskset_attribute_init(&low, a.image, a.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    CHECK(r3 == 0u);
    expected_attribute(expected_a, 0u, 0x00000000u, 0u, priority, 0u);
    CHECK_EQ_MEM(a.image, expected_a, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);

    CHECK(ps3_spurs_taskset_attribute_init(&high, b.image, b.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    CHECK(r3 == 0u);
    expected_attribute(expected_b, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, priority, 0u);
    CHECK_EQ_MEM(b.image, expected_b, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(guarded_intact(&a) && guarded_intact(&b));
}

/* ------------------------------------------------------------------------------------ */
/* Attribute: every error code                                                          */
/* ------------------------------------------------------------------------------------ */

static void test_attribute_null(void)
{
    guarded g;
    uint8_t sentinel[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint32_t r3 = 0;
    spurs_taskset_attribute_request request = {0u, 1u, 0x00250001u, 0u, 0u, 8u};
    guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    memset(sentinel, 0x5A, sizeof(sentinel));
    memcpy(g.image, sentinel, sizeof(sentinel));

    CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_NULL);
    CHECK_EQ_U64(r3, 0x80410911u);
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(r3), 0xFFFFFFFF80410911ull);
    /* A refusal writes no image at all. */
    CHECK_EQ_MEM(g.image, sentinel, sizeof(sentinel));
    CHECK(guarded_intact(&g));
}

static void test_attribute_alignment(void)
{
    /* 0x14480 masks bits 29..31, i.e. `attribute & 7`; every nonzero residue is refused. */
    static const uint32_t bad[7] = {1u, 2u, 3u, 4u, 5u, 6u, 7u};
    for (unsigned i = 0; i < 7; ++i) {
        guarded g;
        uint32_t r3 = 0;
        spurs_taskset_attribute_request request = {0x1000u + bad[i], 1u, 0x00250001u, 0u, 0u, 8u};
        guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
        CHECK_EQ_U64(r3, 0x80410910u);
        CHECK(guarded_intact(&g));
    }
    /* And 8-byte-aligned addresses are accepted. */
    for (unsigned offset = 0; offset < 8; ++offset) {
        guarded g;
        uint32_t r3 = 1u;
        spurs_taskset_attribute_request request = {0x1000u + 8u * 17u, 1u, 0x00250001u, 0u, 0u, 8u};
        (void)offset;
        guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
        CHECK(guarded_intact(&g));
    }
}

static void test_attribute_priority_range(void)
{
    /* 0x144B0 compares each priority byte with 0xF and 0x144B4 branches on `greater`:
     * 0x10..0xFF are refused at every one of the eight positions, and 0x0F is accepted. */
    for (unsigned position = 0; position < 8; ++position) {
        static const uint32_t bad[3] = {0x10u, 0x80u, 0xFFu};
        for (unsigned k = 0; k < 3; ++k) {
            const unsigned shift = 8u * (7u - position);
            guarded g;
            uint32_t r3 = 0;
            spurs_taskset_attribute_request request = {0x1000u, 1u, 0x00250001u, 0u,
                                                       (uint64_t)bad[k] << shift, 8u};
            guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
            CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
                  PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
            CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_INVAL);
            CHECK_EQ_U64(r3, 0x80410902u);
            CHECK(guarded_intact(&g));
        }
    }
    {
        /* All eight at the maximum accepted value. */
        guarded g;
        uint32_t r3 = 1u;
        uint8_t expected[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
        static const uint8_t priority[8] = {0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F};
        spurs_taskset_attribute_request request = {0x1000u, 1u, 0x00250001u, 0u,
                                                   0x0F0F0F0F0F0F0F0Fu, 8u};
        guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
        expected_attribute(expected, 1u, 0x00250001u, 0u, priority, 8u);
        CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        CHECK(guarded_intact(&g));
    }
}

static void test_attribute_validation_order(void)
{
    /* CONTRACT §A.2: NULL beats alignment beats priority. The firmware reaches 0x14480 only
     * after the NULL test, so a NULL attribute never returns ALIGN even with a bad address
     * (a NULL address is by definition aligned, so the order is proved with ALIGN vs INVAL). */
    {
        guarded g;
        uint32_t r3 = 0;
        /* Misaligned AND a bad priority byte: ALIGN must win (0x14490 precedes the loop). */
        spurs_taskset_attribute_request request = {0x1004u, 1u, 0x00250001u, 0u,
                                                   0xFF00000000000000u, 8u};
        guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
    }
    {
        guarded g;
        uint32_t r3 = 0;
        spurs_taskset_attribute_request request = {0u, 1u, 0x00250001u, 0u,
                                                   0xFF00000000000000u, 8u};
        guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        CHECK(ps3_spurs_taskset_attribute_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_NULL);
    }
}

static void test_attribute_bad_arguments(void)
{
    /* A too-small capacity, a NULL image and a NULL request are caller errors and never
     * produce a firmware code. */
    uint8_t image[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint32_t r3 = 0x1234u;
    spurs_taskset_attribute_request request = {0x1000u, 1u, 0x00250001u, 0u, 0u, 8u};
    CHECK(ps3_spurs_taskset_attribute_init(&request, image,
                                           PS3_SPURS_TASKSET_ATTRIBUTE_BYTES - 1u, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK_EQ_U64(r3, 0x1234u); /* untouched */
    CHECK(ps3_spurs_taskset_attribute_init(&request, NULL, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES,
                                           &r3) == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_attribute_init(NULL, image, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES,
                                           &r3) == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    /* A NULL r3 output is allowed: the image is still produced. */
    CHECK(ps3_spurs_taskset_attribute_init(&request, image, sizeof(image), NULL) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
}

/* ------------------------------------------------------------------------------------ */
/* Event flag: success and the exact inFAMOUS image                                     */
/* ------------------------------------------------------------------------------------ */

static void test_event_flag_infamous_image(void)
{
    /* Survey §A.2.3: r3 = the SPURS instance EA the game stored, r4 = 0 (taskset),
     * r5 = 0x2F00 (eventFlag), r6 = 0 (CLEAR_AUTO), r7 = 1 (OR). */
    const uint32_t spurs = 0x0D007A80u; /* 128-byte aligned, as the firmware requires */
    guarded g;
    uint8_t expected[PS3_SPURS_EVENT_FLAG_BYTES];
    uint32_t r3 = 0xDEADBEEFu;
    spurs_event_flag_request request = {spurs, 0u, 0x2F00u, 0u, 1u, 0u};
    guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
    CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    expected_event_flag(expected, spurs, 0u, 0u, 1u);

    CHECK_EQ_U64(r3, 0u);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_EVENT_FLAG_BYTES);
    CHECK(guarded_intact(&g));

    /* The four named fields and the entirely-zero rest, quoted from CONTRACT §C. */
    CHECK(g.image[PS3_SPURS_EVFLAG_SPU_PORT] == 0xFFu);
    CHECK(g.image[PS3_SPURS_EVFLAG_IS_IWL] == 0x01u);
    CHECK(g.image[PS3_SPURS_EVFLAG_DIRECTION] == 0x01u);
    CHECK(g.image[PS3_SPURS_EVFLAG_CLEAR_MODE] == 0x00u);
    check_all_zero(g.image, 0x0C);
    check_all_zero(g.image + 0x10, 0x60);
    check_all_zero(g.image + 0x78, 8);
}

static void test_event_flag_taskset_branch(void)
{
    /* taskset != 0 selects 0x1589C/0x158A0: addr = taskset EA and isIwl stays 0. */
    const uint32_t spurs = 0x0D007A80u, taskset = 0x0D003F80u;
    guarded g;
    uint8_t expected[PS3_SPURS_EVENT_FLAG_BYTES];
    uint32_t r3 = 1u;
    spurs_event_flag_request request = {spurs, taskset, 0x2F80u, 1u, 3u, 0u};
    guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
    CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    expected_event_flag(expected, spurs, taskset, 1u, 3u);
    CHECK_EQ_U64(r3, 0u);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_EVENT_FLAG_BYTES);
    CHECK(g.image[PS3_SPURS_EVFLAG_IS_IWL] == 0u);
    CHECK(guarded_intact(&g));
}

static void test_event_flag_boundaries(void)
{
    /* Every accepted (clear_mode, direction) pair at the documented limits. */
    for (uint32_t clear = 0; clear <= PS3_SPURS_EVENT_FLAG_CLEAR_LAST; ++clear) {
        for (uint32_t direction = 0; direction <= PS3_SPURS_EVENT_FLAG_LAST; ++direction) {
            guarded g;
            uint32_t r3 = 1u;
            spurs_event_flag_request request = {0x0D007A80u, 0u, 0x2F00u, clear, direction, 0u};
            guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
            CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
                  PS3_SPURS_TASKSET_INIT_STATUS_OK);
            CHECK_EQ_U64(r3, 0u);
            CHECK(g.image[PS3_SPURS_EVFLAG_CLEAR_MODE] == (uint8_t)clear);
            CHECK(g.image[PS3_SPURS_EVFLAG_DIRECTION] == (uint8_t)direction);
            CHECK(guarded_intact(&g));
        }
    }
}

/* ------------------------------------------------------------------------------------ */
/* Event flag: every error code                                                         */
/* ------------------------------------------------------------------------------------ */

static void test_event_flag_null(void)
{
    /* CONTRACT §B.2 step 1, branch by branch. With taskset == 0 both spurs and event_flag
     * are required; with taskset != 0 only event_flag is, so a NULL spurs is accepted. */
    const uint32_t spurs = 0x0D007A80u, taskset = 0x0D003F80u;
    struct {
        spurs_event_flag_request request;
        uint32_t expected;
    } cases[] = {
        {{0u, 0u, 0x2F00u, 0u, 1u, 0u}, PS3_SPURS_TASKSET_INIT_NULL},          /* both NULL  */
        {{0u, 0u, 0u, 0u, 1u, 0u}, PS3_SPURS_TASKSET_INIT_NULL},              /* all NULL   */
        {{spurs, 0u, 0u, 0u, 1u, 0u}, PS3_SPURS_TASKSET_INIT_NULL},           /* no object  */
        {{0u, taskset, 0u, 0u, 1u, 0u}, PS3_SPURS_TASKSET_INIT_NULL},         /* no object  */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        guarded g;
        uint8_t sentinel[PS3_SPURS_EVENT_FLAG_BYTES];
        uint32_t r3 = 0;
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        memset(sentinel, 0x3C, sizeof(sentinel));
        memcpy(g.image, sentinel, sizeof(sentinel));
        CHECK(ps3_spurs_event_flag_init(&cases[i].request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, cases[i].expected);
        CHECK_EQ_U64(r3, 0x80410911u);
        CHECK_EQ_MEM(g.image, sentinel, sizeof(sentinel));
        CHECK(guarded_intact(&g));
    }
    /* taskset != 0 with spurs == 0 is ACCEPTED (the firmware never tests spurs on that
     * path: 0x1579C tests only r31). */
    {
        guarded g;
        uint8_t expected[PS3_SPURS_EVENT_FLAG_BYTES];
        uint32_t r3 = 1u;
        spurs_event_flag_request request = {0u, taskset, 0x2F80u, 0u, 1u, 0u};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
        expected_event_flag(expected, 0u, taskset, 0u, 1u);
        CHECK_EQ_MEM(g.image, expected, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(guarded_intact(&g));
    }
}

static void test_event_flag_alignment(void)
{
    /* 0x157EC / 0x15830 / 0x1583C mask bits 25..31: 128-byte alignment, on all three
     * pointers, on both branches (spurs is checked even when taskset == 0). */
    const uint32_t spurs = 0x0D007A80u, taskset = 0x0D003F80u, object = 0x2F00u;
    static const uint32_t bad[4] = {1u, 0x10u, 0x40u, 0x7Fu};
    for (unsigned i = 0; i < 4; ++i) {
        /* spurs misaligned, taskset == 0 (the branch the game takes). */
        {
            guarded g;
            uint32_t r3 = 0;
            spurs_event_flag_request request = {spurs + bad[i], 0u, object, 0u, 1u, 0u};
            guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
            CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
                  PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
            CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
            CHECK_EQ_U64(r3, 0x80410910u);
            CHECK(guarded_intact(&g));
        }
        /* taskset misaligned. */
        {
            guarded g;
            uint32_t r3 = 0;
            spurs_event_flag_request request = {spurs, taskset + bad[i], object, 0u, 1u, 0u};
            guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
            CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
                  PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
            CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
        }
        /* eventFlag misaligned. */
        {
            guarded g;
            uint32_t r3 = 0;
            spurs_event_flag_request request = {spurs, 0u, 0x2F00u + bad[i], 0u, 1u, 0u};
            guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
            CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
                  PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
            CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
        }
    }
}

static void test_event_flag_ranges(void)
{
    const uint32_t spurs = 0x0D007A80u, taskset = 0x0D003F80u, object = 0x2F00u;
    /* direction > 3. */
    for (uint32_t direction = PS3_SPURS_EVENT_FLAG_LAST + 1u; direction < 0x100u; direction += 0x11u) {
        guarded g;
        uint32_t r3 = 0;
        spurs_event_flag_request request = {spurs, 0u, object, 0u, direction, 0u};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_INVAL);
        CHECK_EQ_U64(r3, 0x80410902u);
    }
    /* clear_mode > 1. */
    for (uint32_t clear = PS3_SPURS_EVENT_FLAG_CLEAR_LAST + 1u; clear < 0x100u; clear += 0x11u) {
        guarded g;
        uint32_t r3 = 0;
        spurs_event_flag_request request = {spurs, 0u, object, clear, 0u, 0u};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_INVAL);
    }
    /* taskset != 0 and wid > 0x1F. 0x1F is accepted. */
    {
        guarded g;
        uint32_t r3 = 1u;
        spurs_event_flag_request request = {spurs, taskset, object, 0u, 0u, 0x1Fu};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
        CHECK(guarded_intact(&g));
    }
    for (uint32_t wid = 0x20u; wid < 0x100u; wid += 0x0Du) {
        guarded g;
        uint32_t r3 = 0;
        spurs_event_flag_request request = {spurs, taskset, object, 0u, 0u, wid};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_INVAL);
    }
    /* A huge wid is ignored on the taskset == 0 branch the game takes. */
    {
        guarded g;
        uint32_t r3 = 1u;
        spurs_event_flag_request request = {spurs, 0u, object, 0u, 0u, 0xFFFFFFFFu};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
    }
}

static void test_event_flag_validation_order(void)
{
    /* CONTRACT §B.2: NULL, then alignment, then wid, then direction, then clear mode. */
    const uint32_t spurs = 0x0D007A80u, taskset = 0x0D003F80u;
    {
        /* NULL object beats everything. */
        guarded g;
        uint32_t r3 = 0;
        spurs_event_flag_request request = {spurs + 1u, 0u, 0u, 0xFFu, 0xFFu, 0xFFu};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_NULL);
    }
    {
        /* Misaligned beats an out-of-range wid and an out-of-range direction. */
        guarded g;
        uint32_t r3 = 0;
        spurs_event_flag_request request = {spurs, taskset + 8u, 0x2F00u, 0xFFu, 0xFFu, 0xFFu};
        guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(ps3_spurs_event_flag_init(&request, g.image, g.capacity, &r3) ==
              PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
    }
    /* A bad wid and a bad direction both report INVAL, so the order between them is
     * unobservable in the result: CONTRACT §B.2 says so (they share the 0x15860 exit). */
}

static void test_event_flag_bad_arguments(void)
{
    uint8_t image[PS3_SPURS_EVENT_FLAG_BYTES];
    uint32_t r3 = 0x1234u;
    spurs_event_flag_request request = {0x0D007A80u, 0u, 0x2F00u, 0u, 1u, 0u};
    CHECK(ps3_spurs_event_flag_init(&request, image, PS3_SPURS_EVENT_FLAG_BYTES - 1u, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK_EQ_U64(r3, 0x1234u);
    CHECK(ps3_spurs_event_flag_init(&request, NULL, PS3_SPURS_EVENT_FLAG_BYTES, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_event_flag_init(NULL, image, PS3_SPURS_EVENT_FLAG_BYTES, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_event_flag_init(&request, image, sizeof(image), NULL) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
}

/* ------------------------------------------------------------------------------------ */
/* The struct sizes the firmware touches                                                */
/* ------------------------------------------------------------------------------------ */

static void test_sizes(void)
{
    /* CONTRACT §A.4 / §B.3: the memset lengths, which are also the struct sizes. */
    CHECK_EQ_U64(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 512u);
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_BYTES, 128u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_SIZE, 6400u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_PRIORITY_BYTES, 8u);
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_SPU_PORT_INVALID, 0xFFu);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_ATTRIBUTE_ALIGN, 8u);
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ALIGN, 128u);
    CHECK_EQ_U64(PS3_SPURS_MAX_WORKLOAD2, 32u);
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_LAST, 3u);
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_CLEAR_LAST, 1u);
    /* Every named field lies inside its struct. */
    CHECK(PS3_SPURS_TSATTR_ENABLE_CLEAR_LS + 4u <= PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(PS3_SPURS_EVFLAG_ADDR + 8u <= PS3_SPURS_EVENT_FLAG_BYTES);
}

static void test_sign_extend(void)
{
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(0u), 0u);
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(PS3_SPURS_TASKSET_INIT_NULL),
                 0xFFFFFFFF80410911ull);
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(PS3_SPURS_TASKSET_INIT_ALIGN),
                 0xFFFFFFFF80410910ull);
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(PS3_SPURS_TASKSET_INIT_INVAL),
                 0xFFFFFFFF80410902ull);
}

static void test_put_be(void)
{
    static const uint8_t expect8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t bytes[8];
    memset(bytes, 0, sizeof(bytes));
    CHECK(ps3_spurs_taskset_init_put_be(bytes, sizeof(bytes), 0, 4, 0xDEADBEEFu) == 0);
    CHECK(bytes[0] == 0xDE && bytes[1] == 0xAD && bytes[2] == 0xBE && bytes[3] == 0xEF);
    CHECK(ps3_spurs_taskset_init_put_be(bytes, sizeof(bytes), 0, 8, UINT64_C(0x0102030405060708)) == 0);
    CHECK_EQ_MEM(bytes, expect8, 8);
    /* Out-of-range spans are refused and write nothing. */
    uint8_t guard[8];
    memset(guard, 0x77, sizeof(guard));
    CHECK(ps3_spurs_taskset_init_put_be(guard, 8, 6, 4, 0xFFFFFFFFu) != 0);
    CHECK(ps3_spurs_taskset_init_put_be(guard, 8, 9, 1, 0xFFu) != 0);
    CHECK(ps3_spurs_taskset_init_put_be(guard, 8, 0, 3, 0xFFu) != 0);
    CHECK(guard[6] == 0x77 && guard[7] == 0x77);
    CHECK(ps3_spurs_taskset_init_put_be(NULL, 8, 0, 1, 0u) != 0);
}

/* ------------------------------------------------------------------------------------ */
/* 0x652B70E2: the taskset attribute name setter                          */
/* ------------------------------------------------------------------------------------ */

static void test_attribute_set_name(void)
{
    guarded g;
    uint32_t r3 = 0;
    /* inFAMOUS: r3 = SP+0x90 (the attribute the init just built),
     * r4 = the name EA from *(u32*)(r30-0x7F7C), here 0x0010A4C0. */
    const uint32_t attr = 0x0F2F90u;   /* 8-byte aligned, like SP+0x90 on a 16-byte frame */
    const uint32_t name = 0x0010A4C0u;
    spurs_taskset_attribute_name_request request = {attr, name};

    /* Success: the image is exactly the four big-endian bytes of the name POINTER, and the
     * firmware stores the pointer rather than copying a string. */
    uint8_t expected[PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES];
    be_put(expected, 0, 4, name);
    guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES);
    memset(g.image, 0x5A, g.capacity);
    CHECK(ps3_spurs_taskset_attribute_set_name(&request, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES);
    CHECK(guarded_intact(&g));

    /* The store lands at the layout's name field: CONTRACT g10 §A.4 puts `name` at +0x1C and
     * the init zeroes it, so this entry is what first places a value there. */
    CHECK(PS3_SPURS_TSATTR_NAME == 0x1Cu);
    CHECK(PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES == 4u);
    CHECK(PS3_SPURS_TSATTR_NAME + PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES <=
          PS3_SPURS_TSATTR_TASKSET_SIZE);

    /* ---- refusal: NULL attribute -> 0x80410911, nothing written ---- */
    uint8_t sentinel[PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES];
    memset(sentinel, 0x5A, sizeof(sentinel));
    memset(g.image, 0x5A, g.capacity);
    spurs_taskset_attribute_name_request null_attr = {0u, name};
    CHECK(ps3_spurs_taskset_attribute_set_name(&null_attr, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_NULL);
    CHECK_EQ_U64(r3, 0x80410911u);
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(r3), 0xFFFFFFFF80410911ull);
    CHECK_EQ_MEM(g.image, sentinel, sizeof(sentinel));
    CHECK(guarded_intact(&g));

    /* refusal: NULL name -> the SAME code 0x80410911 */
    memset(g.image, 0x5A, g.capacity);
    spurs_taskset_attribute_name_request null_name = {attr, 0u};
    CHECK(ps3_spurs_taskset_attribute_set_name(&null_name, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);
    CHECK_EQ_MEM(g.image, sentinel, sizeof(sentinel));

    /* ---- refusal: misaligned attribute -> 0x80410910 ---- */
    memset(g.image, 0x5A, g.capacity);
    spurs_taskset_attribute_name_request misaligned = {attr + 4u, name};
    CHECK(ps3_spurs_taskset_attribute_set_name(&misaligned, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
    CHECK_EQ_U64(r3, 0x80410910u);
    CHECK_EQ_MEM(g.image, sentinel, sizeof(sentinel));

    /* ---- ORDER: the name test at 0x14338 precedes the alignment test at 0x14350, so a
     * misaligned attribute with a NULL name reports 0x80410911, not 0x80410910. ---- */
    spurs_taskset_attribute_name_request both = {attr + 4u, 0u};
    CHECK(ps3_spurs_taskset_attribute_set_name(&both, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);

    /* ---- boundaries: every 8-byte-aligned attribute is accepted ---- */
    for (uint32_t offset = 0; offset < 8u; ++offset) {
        spurs_taskset_attribute_name_request probe = {0x1000u + offset * 8u, name};
        const spurs_taskset_init_status status =
            ps3_spurs_taskset_attribute_set_name(&probe, g.image, g.capacity, &r3);
        CHECK(status == PS3_SPURS_TASKSET_INIT_STATUS_OK);
        CHECK_EQ_U64(r3, 0u);
        CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES);
    }
    /* One byte off is refused. */
    spurs_taskset_attribute_name_request off = {0x1001u, name};
    CHECK(ps3_spurs_taskset_attribute_set_name(&off, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410910u);

    /* ---- bad arguments: NULL request/out and a short capacity ---- */
    CHECK(ps3_spurs_taskset_attribute_set_name(NULL, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_attribute_set_name(&request, NULL, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_attribute_set_name(&request, g.image, 3u, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    /* A NULL r3 is allowed. */
    CHECK(ps3_spurs_taskset_attribute_set_name(&request, g.image, g.capacity, NULL) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
}

/* ------------------------------------------------------------------------------------ */
/* 0x87630976: the event-flag attach to an lv2 queue (g12 CONTRACT §A-§C)               */
/* ------------------------------------------------------------------------------------ */

static void test_event_flag_attach(void)
{
    uint32_t r3 = 0;
    /* Gate values the native _cellSpursEventFlagInitialize produces for inFAMOUS: spuPort 0xFF,
     * isIwl 1, direction 1 (CONTRACT g10 §C). */
    const uint32_t flag = 0x00007000u;  /* 128-byte aligned */

    /* ---- the firmware's inFAMOUS call, asserted field by field: direction 1 / isIwl 1 with
     * the init's own spuPort 0xFF. Every gate clears, so the unit hands
     * the continuation over and leaves r3 at 0. g12c: this is now the SERVED path (the g12b
     * packet decode + CONTRACT-DYNAMIC.md's dynamic connect removed both blockers), so the
     * status is ATTACH_QUEUE, not the retired UNSUPPORTED stop. ---- */
    spurs_event_flag_attach_request infamous = {0x00005880u, 1u, 0xFFu, 1u, 0u};   /* r3 = 0x5880 */
    CHECK_EQ_U64(infamous.event_flag, 0x5880u);
    CHECK_EQ_U64(infamous.direction, PS3_SPURS_EVENT_FLAG_ATTACH_OR);   /* r7 = 1 at 0x000F2E48 */
    CHECK_EQ_U64(infamous.is_iwl, 1u);                                 /* 0x158BC taskset==NULL  */
    r3 = 0xA5A5A5A5u;
    CHECK(ps3_spurs_event_flag_attach(&infamous, &r3) == PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE);
    CHECK_EQ_U64(r3, 0u);
    /* SELECTOR: gate 5's operand choice. isIwl 1 selects the word at F+0x74, the
     * inFAMOUS path; the gate result is identical for isIwl 0, which is why the field is
     * carried in the request even though no gate reads it. */
    { spurs_event_flag_attach_request iwl0 = infamous; iwl0.is_iwl = 0u;
      CHECK(ps3_spurs_event_flag_attach(&iwl0, &r3) == PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE);
      CHECK_EQ_U64(r3, 0u); }

    /* ---- EXACT WRITES on the direction-1 / isIwl 1 path (g12 §C.2): the firmware's two stores
     * are F+0x7C (4 bytes, the queue id) and F+0x0C (1 byte, the bound port). This unit performs
     * NO write (it produces no image and touches no guest memory): the ADAPTER makes them, and
     * the startup case measures the exact bytes and their order. The offsets are asserted so a
     * later slice cannot silently retarget them. ---- */
    CHECK_EQ_U64(PS3_SPURS_EVFLAG_SPU_PORT, 0x0Cu);
    CHECK_EQ_U64(PS3_SPURS_EVFLAG_IS_IWL, 0x0Du);
    CHECK_EQ_U64(PS3_SPURS_EVFLAG_DIRECTION, 0x0Eu);
    CHECK_EQ_U64(PS3_SPURS_EVFLAG_TASKSET_PTR, 0x74u);
    CHECK_EQ_U64(PS3_SPURS_EVFLAG_QUEUE_ID, 0x7Cu);
    CHECK_EQ_U64(PS3_SPURS_EVFLAG_PORT_ID, 0x78u);

    /* the continuation's packet, decoded from 0xB244.
     * Every field is one ps3_lv2_queue_create accepts, which is WHY this path is servable. ---- */
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_PROTOCOL, 1u);   /* 0xB264: PS3_SYNC_FIFO  */
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_TYPE, 1u);       /* 0xB268: PS3_PPU_QUEUE  */
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_IPC_KEY, 0u);
    CHECK_EQ_U64((unsigned)PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_DEPTH, 1u);
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_NAME, 0x5F73707545764600ull);   /* "_spuEvF\0" */

    /* ---- the dynamic mask libsre builds at 0xAFDC..0xB038: ports 16..63, every bit set. The value is asserted as a
     * literal because this unit is platform-free and cannot include spurs_instance.h (where PS3_SPURS_DYNAMIC_PORT_MASK
     * lives); the startup case checks the two agree. ---- */
    CHECK_EQ_U64(0xFFFFFFFFFFFF0000ull, 0xFFFFFFFFFFFF0000ull);

    /* ---- the error collapse, 0x159A4..0x159BC: bits 16..19 == 0x4 -> 0x80410900 | (code & 0xFF);
     * anything else passes through; 0 stays 0. ---- */
    CHECK_EQ_U64(ps3_spurs_event_flag_attach_collapse(0u), 0u);
    CHECK_EQ_U64(ps3_spurs_event_flag_attach_collapse(0x8041070Au), 0x8041070Au);            /* EISCONN, unchanged */
    CHECK_EQ_U64(ps3_spurs_event_flag_attach_collapse(0x80010005u), 0x80010005u);            /* unchanged */
    CHECK_EQ_U64(ps3_spurs_event_flag_attach_collapse(0x80410711u), 0x80410711u);            /* unchanged */
    CHECK_EQ_U64(ps3_spurs_event_flag_attach_collapse(0x80044215u), 0x80410915u);            /* collapsed  */
    CHECK_EQ_U64(ps3_spurs_event_flag_attach_collapse(0x800444FFu), 0x804109FFu);            /* collapsed  */

    /* ---- the four gates pass -> ATTACH_QUEUE, r3 at 0. ---- */
    spurs_event_flag_attach_request ok = {flag, 1u, 0xFFu, 1u, 0u};
    r3 = 0xDEADBEEFu;
    CHECK(ps3_spurs_event_flag_attach(&ok, &r3) == PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE);
    CHECK_EQ_U64(r3, 0u);
    /* The other accepted direction (ANY2ANY = 3) passes the same way. */
    ok.direction = 3u;
    CHECK(ps3_spurs_event_flag_attach(&ok, &r3) == PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE);

    /* gate 1: NULL object -> 0x80410911 */
    spurs_event_flag_attach_request null_flag = {0u, 1u, 0xFFu, 1u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&null_flag, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);
    CHECK_EQ_U64(ps3_spurs_taskset_init_sign_extend(r3), 0xFFFFFFFF80410911ull);
    /* The NULL gate precedes everything: even garbage field bytes report it. */
    spurs_event_flag_attach_request null_garbage = {0u, 0u, 0u, 0u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&null_garbage, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);

    /* gate 2: alignment & 0x7F -> 0x80410910 */
    spurs_event_flag_attach_request misaligned = {flag + 1u, 1u, 0xFFu, 1u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&misaligned, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410910u);
    for (uint32_t offset = 1; offset < 128u; offset += 1u) {
        spurs_event_flag_attach_request probe = {flag + offset, 1u, 0xFFu, 1u, 0u};
        CHECK(ps3_spurs_event_flag_attach(&probe, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_TASKSET_INIT_ALIGN);
    }

    /* gate 3: direction not in {1, 3} -> 0x80410909 */
    static const unsigned refused_directions[] = {0u, 2u, 4u, 0x7Fu, 0x80u, 0xFEu, 0xFFu};
    for (size_t i = 0; i < sizeof(refused_directions) / sizeof(refused_directions[0]); ++i) {
        spurs_event_flag_attach_request probe = {flag, (uint8_t)refused_directions[i], 0xFFu, 1u, 0u};
        CHECK(ps3_spurs_event_flag_attach(&probe, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_EVENT_FLAG_ATTACH_NO_WAIT);
    }
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ATTACH_NO_WAIT, 0x80410909u);
    /* `OR` = 0 fails: the firmware's accepted set is wider than the init's 0..3 range. */
    spurs_event_flag_attach_request or_dir = {flag, 0u, 0xFFu, 1u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&or_dir, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410909u);

    /* gate 4: spuPort != 0xFF -> 0x8041090F */
    static const unsigned refused_ports[] = {0u, 1u, 0x7Fu, 0x80u, 0xFEu};
    for (size_t i = 0; i < sizeof(refused_ports) / sizeof(refused_ports[0]); ++i) {
        spurs_event_flag_attach_request probe = {flag, 1u, (uint8_t)refused_ports[i], 1u, 0u};
        CHECK(ps3_spurs_event_flag_attach(&probe, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
        CHECK_EQ_U64(r3, PS3_SPURS_EVENT_FLAG_ATTACH_SPU_PORT);
    }
    CHECK_EQ_U64(PS3_SPURS_EVENT_FLAG_ATTACH_SPU_PORT, 0x8041090Fu);
    /* 0 is the memset's value, so an un-initialised flag fails this gate, not gate 3. */
    spurs_event_flag_attach_request zero_port = {flag, 1u, 0u, 1u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&zero_port, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x8041090Fu);

    /* ORDER, measured from the listing: NULL -> align
     * direction -> spuPort. Every later gate's value is ignored by an
     * earlier failure. ---- */
    spurs_event_flag_attach_request all_bad = {flag + 1u, 0u, 0u, 0u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&all_bad, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410910u);
    spurs_event_flag_attach_request dir_before_port = {flag, 0u, 0u, 1u, 0u};
    CHECK(ps3_spurs_event_flag_attach(&dir_before_port, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410909u);

    /* ---- bad arguments ---- */
    CHECK(ps3_spurs_event_flag_attach(NULL, &r3) == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT);
    /* A NULL r3 is allowed on both outcomes. */
    CHECK(ps3_spurs_event_flag_attach(&ok, NULL) == PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE);
    CHECK(ps3_spurs_event_flag_attach(&null_flag, NULL) == PS3_SPURS_TASKSET_INIT_STATUS_REFUSED);
}

/* ------------------------------------------------------------------------------------ */
/* Idempotence: the firmware has no accumulated state                                   */
/* ------------------------------------------------------------------------------------ */

static void test_idempotent(void)
{
    /* A second call with the same arguments produces byte-identical output over a
     * deliberately dirty buffer -- which also proves the builder zero-fills everywhere the
     * firmware memset does. */
    static const uint8_t priority[8] = {4, 5, 6, 7, 8, 9, 10, 11};
    guarded g;
    uint8_t expected_attr[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES];
    uint8_t expected_flag[PS3_SPURS_EVENT_FLAG_BYTES];
    uint32_t r3 = 0;
    spurs_taskset_attribute_request attr = {0x4000u, 2u, 0x00250001u, 0x1122334455667788u,
                                            0x0405060708090A0Bu, 12u};
    spurs_event_flag_request flag = {0x0D007A80u, 0u, 0x2F00u, 1u, 2u, 0u};
    guarded_init(&g, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    CHECK(ps3_spurs_taskset_attribute_init(&attr, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    expected_attribute(expected_attr, 2u, 0x00250001u, 0x1122334455667788u, priority, 12u);
    CHECK_EQ_MEM(g.image, expected_attr, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);

    memset(g.image, 0xFF, g.capacity);
    CHECK(ps3_spurs_taskset_attribute_init(&attr, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    CHECK_EQ_MEM(g.image, expected_attr, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);

    guarded_init(&g, PS3_SPURS_EVENT_FLAG_BYTES);
    CHECK(ps3_spurs_event_flag_init(&flag, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    expected_event_flag(expected_flag, 0x0D007A80u, 0u, 1u, 2u);
    CHECK_EQ_MEM(g.image, expected_flag, PS3_SPURS_EVENT_FLAG_BYTES);
    memset(g.image, 0xFF, g.capacity);
    CHECK(ps3_spurs_event_flag_init(&flag, g.image, g.capacity, &r3) ==
          PS3_SPURS_TASKSET_INIT_STATUS_OK);
    CHECK_EQ_MEM(g.image, expected_flag, PS3_SPURS_EVENT_FLAG_BYTES);
    CHECK(guarded_intact(&g));
}

/* ------------------------------------------------------------------------------------ */
/* cellSpursCreateTasksetWithAttribute (NID 0xC10931CB)               */
/* ------------------------------------------------------------------------------------ */

/* inFAMOUS's own attribute at 0x000F29B0: SP+0x90 built by _cellSpursTasksetAttributeInitialize
 * with revision 1, sdk 0x00250001, args 0, priority {8,0,...}, max_contention 8, taskset_size
 * 6400, name set by cellSpursTasksetAttributeSetName, and enable_clear_ls left zero.
 * The game's spurs object is 128-byte aligned and so is the taskset at 0x2F00 + 0x1080. */
#define G13_SPURS 0x0D007A80u
#define G13_TASKSET 0x0D003F80u
#define G13_ATTR 0x0D00FF90u

static void g13_infamous(spurs_taskset_create_request *request)
{
    memset(request, 0, sizeof(*request));
    request->spurs = G13_SPURS;
    request->taskset = G13_TASKSET;
    request->attr = G13_ATTR;
    request->attribute.revision = 1u;
    request->attribute.args = 0u;
    request->attribute.priority[0] = 8u;
    request->attribute.max_contention = 8u;
    request->attribute.name = 0x0D011111u;
    request->attribute.taskset_size = PS3_SPURS_TASKSET_SIZE;
    request->attribute.enable_clear_ls_byte = 0u;
}

/* 0x14B50 memset + 0x14B90/0x14B94/0x14B98/0x14B9C, field by field from CONTRACT §A.4. */
static void g13_expected_taskset(uint8_t out[PS3_SPURS_TASKSET_BYTES], uint32_t spurs,
                                 uint64_t args, uint8_t enable_clear_ls, uint32_t size)
{
    memset(out, 0, PS3_SPURS_TASKSET_BYTES);
    be_put(out, 0x60, 8, spurs);
    be_put(out, 0x68, 8, args);
    out[0x70] = enable_clear_ls;
    be_put(out, 0x1890, 4, size);
    /* +0x72 and +0x74 stay zero: the adapter writes them after the registration. */
}

typedef struct g13_guarded {
    uint8_t storage[GUARD + PS3_SPURS_TASKSET_BYTES + GUARD];
    uint8_t *image;
    size_t capacity;
} g13_guarded;

static void g13_guarded_init(g13_guarded *g)
{
    memset(g->storage, 0xA5, sizeof(g->storage));
    g->image = g->storage + GUARD;
    g->capacity = PS3_SPURS_TASKSET_BYTES;
}

static int g13_guarded_intact(const g13_guarded *g)
{
    for (size_t i = 0; i < GUARD; ++i) {
        if (g->storage[i] != 0xA5) return 0;
        if (g->storage[GUARD + g->capacity + i] != 0xA5) return 0;
    }
    return 1;
}

static void test_create_validate(void)
{
    spurs_taskset_create_request request;
    spurs_taskset_create_request null_request;
    uint32_t r3 = 0x1234u;
    g13_infamous(&request);

    /* inFAMOUS's values clear all three gates. */
    CHECK(ps3_spurs_taskset_create_validate(&request, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);

    /* attr == NULL -> 0x80410911. */
    null_request = request;
    null_request.attr = 0u;
    null_request.attribute.revision = 7u;   /* an unreadable revision must not matter */
    CHECK(ps3_spurs_taskset_create_validate(&null_request, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
    CHECK_EQ_U64(r3, 0x80410911u);

    /* attr & 7 -> 0x80410910, before the revision read. */
    for (uint32_t misalign = 1; misalign < 8u; ++misalign) {
        spurs_taskset_create_request bad = request;
        bad.attr = G13_ATTR + misalign;
        bad.attribute.revision = 3u;
        r3 = 0x1234u;
        CHECK(ps3_spurs_taskset_create_validate(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410910u);
    }

    /* attr->revision != 1 -> 0x80410902 (every value including 0 and 2). */
    {
        static const uint32_t revisions[] = {0u, 2u, 3u, 0xFFFFFFFFu};
        for (size_t i = 0; i < sizeof(revisions) / sizeof(revisions[0]); ++i) {
            spurs_taskset_create_request bad = request;
            bad.attribute.revision = revisions[i];
            r3 = 0x1234u;
            CHECK(ps3_spurs_taskset_create_validate(&bad, &r3) ==
                  PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
            CHECK_EQ_U64(r3, 0x80410902u);
        }
    }

    CHECK(ps3_spurs_taskset_create_validate(NULL, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_validate(&request, NULL) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
}

static void test_create_arguments(void)
{
    spurs_taskset_create_request request;
    uint32_t r3 = 0;
    g13_infamous(&request);
    CHECK(ps3_spurs_taskset_create_validate_arguments(&request, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);
    /* The entry does not validate spurs/taskset, so these values reach the worker as given. */
    {
        spurs_taskset_create_request bad = request;
        bad.spurs = 0u;
        CHECK(ps3_spurs_taskset_create_validate(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_OK);
        CHECK(ps3_spurs_taskset_create_validate_arguments(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410911u);
        bad = request;
        bad.taskset = 0u;
        CHECK(ps3_spurs_taskset_create_validate_arguments(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410911u);
    }
    /* 128-byte alignment on both, and on the spurs side first. */
    {
        spurs_taskset_create_request bad = request;
        bad.taskset = G13_TASKSET + 0x10u;
        CHECK(ps3_spurs_taskset_create_validate_arguments(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410910u);
        bad = request;
        bad.spurs = G13_SPURS + 0x7Fu;
        CHECK(ps3_spurs_taskset_create_validate_arguments(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410910u);
        /* 0x7F is the mask, not the granularity: |0x7F| passes, |0x80| also passes. */
        bad = request;
        bad.spurs = G13_SPURS - 0x80u;
        CHECK(ps3_spurs_taskset_create_validate_arguments(&bad, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    }
}

static void test_create_image(void)
{
    spurs_taskset_create_request request;
    g13_guarded g;
    uint8_t expected[PS3_SPURS_TASKSET_BYTES];
    uint8_t await_byte[1] = {0};
    uint32_t r3 = 0;
    g13_infamous(&request);

    g13_guarded_init(&g);
    CHECK(ps3_spurs_taskset_create_image(&request, g.image, g.capacity, await_byte, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);
    g13_expected_taskset(expected, G13_SPURS, 0u, 0u, PS3_SPURS_TASKSET_SIZE);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_BYTES);
    CHECK(g13_guarded_intact(&g));
    /* 0x14BD8 is the success-path store; the builder hands the byte over, not into the image. */
    CHECK_EQ_U64(await_byte[0], 0x80u);

    /* A dirty buffer is fully overwritten: the builder zero-fills exactly the 6400 bytes the
     * firmware memset covers, so every byte outside the five stores is 0. */
    memset(g.image, 0xFF, g.capacity);
    CHECK(ps3_spurs_taskset_create_image(&request, g.image, g.capacity, await_byte, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_BYTES);
    CHECK(g13_guarded_intact(&g));

    /* enable_clear_ls is normalised to 0/1 from byte +0x27. */
    {
        static const unsigned char raws[] = {0x00u, 0x01u, 0x02u, 0x7Fu, 0x80u, 0xFFu};
        for (size_t i = 0; i < sizeof(raws) / sizeof(raws[0]); ++i) {
            request.attribute.enable_clear_ls_byte = raws[i];
            g13_guarded_init(&g);
            CHECK(ps3_spurs_taskset_create_image(&request, g.image, g.capacity, await_byte, &r3) ==
                  PS3_SPURS_TASKSET_CREATE_STATUS_OK);
            g13_expected_taskset(expected, G13_SPURS, 0u, raws[i] ? 1u : 0u,
                                 PS3_SPURS_TASKSET_SIZE);
            CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_BYTES);
        }
        request.attribute.enable_clear_ls_byte = 0u;
    }

    /* The other five fields land where CONTRACT §A.4 says and nowhere else. */
    request.attribute.args = UINT64_C(0x1122334455667788);
    request.attribute.taskset_size = 0x1900u;
    request.spurs = 0x0D007B00u;   /* still 128-aligned */
    request.attribute.enable_clear_ls_byte = 0x05u;
    g13_guarded_init(&g);
    CHECK(ps3_spurs_taskset_create_image(&request, g.image, g.capacity, await_byte, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    g13_expected_taskset(expected, 0x0D007B00u, UINT64_C(0x1122334455667788), 1u, 0x1900u);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_TASKSET_BYTES);
    /* +0x74 (wid) and +0x72 (wkl_flag_wait_task) are not part of the image. */
    CHECK_EQ_U64((unsigned)g.image[0x72], 0u);
    CHECK_EQ_U64((unsigned)g.image[0x74], 0u);

    /* Refusals write nothing at all. */
    {
        spurs_taskset_create_request bad = request;
        bad.spurs = 0u;
        g13_guarded_init(&g);
        CHECK(ps3_spurs_taskset_create_image(&bad, g.image, g.capacity, await_byte, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410911u);
        CHECK(g13_guarded_intact(&g));
        for (size_t i = 0; i < PS3_SPURS_TASKSET_BYTES; ++i) CHECK_EQ_U64(g.image[i], 0xA5u);
        bad = request;
        bad.taskset = G13_TASKSET + 4u;
        CHECK(ps3_spurs_taskset_create_image(&bad, g.image, g.capacity, await_byte, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
        CHECK_EQ_U64(r3, 0x80410910u);
        for (size_t i = 0; i < PS3_SPURS_TASKSET_BYTES; ++i) CHECK_EQ_U64(g.image[i], 0xA5u);
    }

    /* Capacity and NULL handling. */
    CHECK(ps3_spurs_taskset_create_image(&request, g.image, PS3_SPURS_TASKSET_BYTES - 1u,
                                         await_byte, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_image(NULL, g.image, g.capacity, await_byte, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_image(&request, NULL, g.capacity, await_byte, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_image(&request, g.image, g.capacity, await_byte, NULL) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(g13_guarded_intact(&g));
}

static void test_create_wid_and_flag_images(void)
{
    uint8_t bytes[8];
    memset(bytes, 0xA5, sizeof(bytes));
    CHECK(ps3_spurs_taskset_create_wid_image(0u, bytes, sizeof(bytes)) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(bytes[0], 0u);
    CHECK_EQ_U64(bytes[4], 0xA5u);
    CHECK(ps3_spurs_taskset_create_wid_image(0x01020304u, bytes, sizeof(bytes)) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(bytes[0], 0x01u);
    CHECK_EQ_U64(bytes[1], 0x02u);
    CHECK_EQ_U64(bytes[2], 0x03u);
    CHECK_EQ_U64(bytes[3], 0x04u);
    CHECK_EQ_U64(bytes[4], 0xA5u);
    CHECK(ps3_spurs_taskset_create_wid_image(1u, bytes, 3u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_wid_image(1u, NULL, 4u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);

    memset(bytes, 0xA5, sizeof(bytes));
    CHECK(ps3_spurs_taskset_create_flag_wait_image(bytes, sizeof(bytes)) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(bytes[0], 0x80u);
    CHECK_EQ_U64(bytes[1], 0xA5u);
    CHECK(ps3_spurs_taskset_create_flag_wait_image(bytes, 0u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_flag_wait_image(NULL, 1u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
}

/* 0xDC5C's store: `taskset + 0x100 + wid*128`, 8 bytes, value 1
 */
static void test_create_flag_store(void)
{
    uint8_t bytes[8];
    uint32_t address = 0xDEADBEEFu;

    /* wid <= 15 addresses the first flag array. */
    CHECK_EQ_U64(PS3_SPURS_CSTS_WKL_FLAG_BASE, 0x100u);
    CHECK_EQ_U64(PS3_SPURS_CSTS_WKL_FLAG2_BASE, 0xB00u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_WKL_FLAG_STRIDE, 128u);
    for (uint32_t wid = 0; wid <= 15u; ++wid) {
        address = 0xDEADBEEFu;
        CHECK(ps3_spurs_taskset_create_flag_address(G13_TASKSET, wid, &address) == 0);
        CHECK_EQ_U64(address, G13_TASKSET + 0x100u + wid * 128u);
    }
    /* The second form:
     * wid > 15 addresses wklFlag2 with (wid & 15). */
    for (uint32_t wid = 16u; wid <= 31u; ++wid) {
        address = 0xDEADBEEFu;
        CHECK(ps3_spurs_taskset_create_flag_address(G13_TASKSET, wid, &address) == 0);
        CHECK_EQ_U64(address, G13_TASKSET + 0xB00u + (wid & 15u) * 128u);
    }
    /* Every address the two forms can produce lies inside the 6400-byte object the call wrote. */
    for (uint32_t wid = 0; wid < 64u; ++wid) {
        CHECK(ps3_spurs_taskset_create_flag_address(G13_TASKSET, wid, &address) == 0);
        CHECK(address + 8u <= G13_TASKSET + PS3_SPURS_TASKSET_BYTES);
    }
    CHECK(ps3_spurs_taskset_create_flag_address(G13_TASKSET, 0u, NULL) != 0);

    /* an 8-byte big-endian 1 and nothing else. */
    memset(bytes, 0xA5, sizeof(bytes));
    CHECK(ps3_spurs_taskset_create_flag_value_image(bytes, sizeof(bytes)) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    for (size_t i = 0; i < 7u; ++i) CHECK_EQ_U64(bytes[i], 0u);
    CHECK_EQ_U64(bytes[7], 1u);
    CHECK(ps3_spurs_taskset_create_flag_value_image(bytes, 7u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_flag_value_image(NULL, 8u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
}

static void test_create_registration(void)
{
    spurs_taskset_create_request request;
    spurs_taskset_registration registration;
    uint32_t r3 = 0x1234u;
    g13_infamous(&request);

    CHECK(ps3_spurs_taskset_create_registration(&request, 0x00030FA8u, &registration, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(r3, 0u);
    CHECK_EQ_U64(registration.spurs, G13_SPURS);
    /* data is the TASKSET EA (r21 = the low 32 bits of the worker's r4).
     * g13 registered attribute->args here; that was a mis-read of 0xA048 (0xA000's own callee-saved
     * r26 = ITS r7 = the size argument). */
    CHECK_EQ_U64(registration.data, G13_TASKSET);
    CHECK_EQ_U64(registration.priority[0], 8u);
    for (unsigned i = 1; i < 8u; ++i) CHECK_EQ_U64(registration.priority[i], 0u);
    /* min_contention is the literal, not attribute->max_contention. */
    CHECK_EQ_U64(registration.min_contention, 8u);
    CHECK_EQ_U64(registration.max_contention, 8u);
    /* The two decoded constants. */
    CHECK_EQ_U64(registration.policy_module, 0x00023680u);
    CHECK_EQ_U64(registration.size, 0x1E40u);
    CHECK_EQ_U64(registration.name_class, 0x00030FA8u);
    CHECK_EQ_U64(registration.name_instance, 0x0D011111u);
    CHECK_EQ_U64(registration.hook, 0u);
    CHECK_EQ_U64(registration.hook_argument, 0u);

    /* min_contention stays 8 whatever the attribute says; max_contention is carried through
     * unbounded and clamped later by the registration domain (0x97E4's `> 8 ? 8 :`). */
    request.attribute.max_contention = 0xFFFFFFFFu;
    CHECK(ps3_spurs_taskset_create_registration(&request, 0u, &registration, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK_EQ_U64(registration.min_contention, 8u);
    CHECK_EQ_U64(registration.max_contention, 0xFFFFFFFFu);
    request.attribute.max_contention = 8u;

    /* 0xA0E8..0xA0F0/0xA14C: any priority byte above 0xF -> 0x80410802 before anything else. */
    {
        static const uint8_t bad_values[] = {0x10u, 0x20u, 0x7Fu, 0x80u, 0xF0u, 0xFFu};
        for (size_t i = 0; i < sizeof(bad_values) / sizeof(bad_values[0]); ++i) {
            spurs_taskset_create_request bad = request;
            bad.attribute.priority[3] = bad_values[i];
            bad.attribute.priority[7] = bad_values[i];
            r3 = 0x1234u;
            CHECK(ps3_spurs_taskset_create_registration(&bad, 0u, &registration, &r3) ==
                  PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED);
            CHECK_EQ_U64(r3, 0x80410802u);
        }
        spurs_taskset_create_request good = request;
        for (unsigned i = 0; i < 8u; ++i) good.attribute.priority[i] = 0xFu;
        CHECK(ps3_spurs_taskset_create_registration(&good, 0u, &registration, &r3) ==
              PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    }

    CHECK(ps3_spurs_taskset_create_registration(NULL, 0u, &registration, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_registration(&request, 0u, NULL, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_registration(&request, 0u, &registration, NULL) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
}

static void test_create_workload_attribute_image(void)
{
    spurs_taskset_create_request request;
    spurs_taskset_registration registration;
    uint8_t image[PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES];
    uint8_t expected[PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES];
    uint32_t r3 = 0;
    g13_infamous(&request);
    request.attribute.args = UINT64_C(0x0102030405060708);
    request.attribute.priority[0] = 5u;
    request.attribute.priority[7] = 9u;
    CHECK(ps3_spurs_taskset_create_registration(&request, 0x0003176Cu, &registration, &r3) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    CHECK(ps3_spurs_taskset_create_workload_attribute_image(&registration, image,
                                                            sizeof(image)) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_OK);
    /* 0xA11C..0xA144 in the continuation's order, transcribed from CONTRACT §B.3. */
    memset(expected, 0, sizeof(expected));
    be_put(expected, 0x00, 4, 1u);
    be_put(expected, 0x04, 4, 0x00330000u);
    be_put(expected, 0x08, 4, 0x00023680u);
    be_put(expected, 0x0C, 4, 0x00001E40u);
    be_put(expected, 0x10, 8, G13_TASKSET);
    expected[0x18] = 5u;
    expected[0x1F] = 9u;
    be_put(expected, 0x20, 4, 8u);
    be_put(expected, 0x24, 4, 8u);
    be_put(expected, 0x28, 4, 0x0003176Cu);
    be_put(expected, 0x2C, 4, 0x0D011111u);
    CHECK_EQ_MEM(image, expected, PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES);
    CHECK(ps3_spurs_taskset_create_workload_attribute_image(NULL, image, sizeof(image)) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_taskset_create_workload_attribute_image(&registration, image, 511u) ==
          PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT);
}

static void test_create_collapse(void)
{
    /* 0x14C74..0x14CF4 plus the pass-through arm at 0x14C94. */
    CHECK(ps3_spurs_taskset_create_classify(0u) == PS3_SPURS_TASKSET_CREATE_PASS);
    CHECK(ps3_spurs_taskset_create_classify(0x80410811u) == PS3_SPURS_TASKSET_CREATE_PASS);
    CHECK(ps3_spurs_taskset_create_classify(0x80410810u) == PS3_SPURS_TASKSET_CREATE_PASS);
    CHECK(ps3_spurs_taskset_create_classify(0x8041080Fu) == PS3_SPURS_TASKSET_CREATE_STAT_);
    CHECK(ps3_spurs_taskset_create_classify(0x80410802u) == PS3_SPURS_TASKSET_CREATE_INVAL_);
    CHECK(ps3_spurs_taskset_create_classify(0x80410801u) == PS3_SPURS_TASKSET_CREATE_AGAIN_);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0u), 0u);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x8041080Fu), 0x8041090Fu);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x80410802u), 0x80410902u);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x80410801u), 0x80410901u);
    /* Only these three are remapped; these four pass through. */
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x80410811u), 0x80410811u);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x80410810u), 0x80410810u);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x80410804u), 0x80410804u);
    CHECK_EQ_U64(ps3_spurs_taskset_create_registration_code(0x80410901u), 0x80410901u);
    /* The two publish arms (the success arm and the 0x80410910 test). */
    CHECK(ps3_spurs_taskset_create_publishes_workload(0u) == 1);
    CHECK(ps3_spurs_taskset_create_publishes_workload(0x80410910u) == 1);
    CHECK(ps3_spurs_taskset_create_publishes_workload(0x80410911u) == 0);
    CHECK(ps3_spurs_taskset_create_publishes_workload(0x80410811u) == 0);
    CHECK(ps3_spurs_taskset_create_publishes_workload(0x80410901u) == 0);
    CHECK(ps3_spurs_taskset_create_publishes_workload(0x80410902u) == 0);
    CHECK(ps3_spurs_taskset_create_publishes_workload(0x8041090Fu) == 0);
}

static void test_create_constants(void)
{
    /* The decoded constants of CONTRACT §B.3 and §C.2. */
    CHECK_EQ_U64(PS3_SPURS_TASKSET_BYTES, 6400u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_PM_EA, 0x00023680u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_PM_SIZE, 0x1E40u);
    /* The PM values are the pm/size pair, and data is the taskset EA: they are three different
     * numbers, which is the whole point of g13b. */
    CHECK(G13_TASKSET != PS3_SPURS_TASKSET_PM_EA && G13_TASKSET != PS3_SPURS_TASKSET_PM_SIZE);
    CHECK_EQ_U64(PS3_SPURS_WORKLOAD_SDK_VERSION, 0x00330000u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET_WKL_FLAG_WAIT_TASK, 0x80u);
    CHECK_EQ_U64(PS3_SPURS_TASKSET2_SIZE_THRESHOLD, 0x28FFu);
    CHECK_EQ_U64(PS3_SPURS_CSTS_WID, 0x74u);
    CHECK_EQ_U64(PS3_SPURS_CSTS_SIZE, 0x1890u);
    CHECK_EQ_U64(PS3_SPURS_CSTS_EVENT_FLAG_SET_WORD, 0x70u);
    CHECK_EQ_U64(ps3_spurs_taskset_create_sign_extend(0x80410911u),
                 UINT64_C(0xFFFFFFFF80410911));
}

/* ==================================================================================== */
/* g14/g14b: _cellSpursLFQueueInitialize (NID 0x011EE38B)                               */
/* ==================================================================================== */

/* inFAMOUS's own call, decoded from the recompiled site at 0x004248B0..0x004248D4:
 *   r3 = taskset | 1   (tagged owner)
 *   r4 = taskset       (the same object)
 *   r5 = taskset + 0x80
 *   r6 = 0x20
 *   r7 = 3
 *   r8 = 3
 * The game's taskset is the object cellSpursCreateTasksetWithAttribute built, whose +0x24 is the
 * version word the g13 image leaves at 0 (the memsets zero it and nothing writes it), so the
 * shape selected is the CellSyncLFQueue one. Its +0x2C state word is also 0, which is what makes
 * the entry take the builder branch. `LFQ_GAME_TASKSET` is a plausible 128-aligned EA. */
#define LFQ_GAME_TASKSET 0x3F80u   /* 128-aligned, inside the game's 16 MiB window */

static void lfq_game_request(spurs_lfq_request *request)
{
    request->owner = LFQ_GAME_TASKSET | 1u;
    request->queue = LFQ_GAME_TASKSET;
    request->buffer = LFQ_GAME_TASKSET + 0x80u;
    request->elem_size = 0x20u;
    request->depth = 3u;
    request->direction = 3u;
}

/* The expected 128-byte line, written field by field from CONTRACT §B.3 -- never captured from
 * the implementation. `spurs_shape` selects the +0x2050 tail. */
static void lfq_expected_line(uint8_t out[PS3_SPURS_LFQ_BYTES], const spurs_lfq_request *r,
                              int spurs_shape)
{
    memset(out, 0, PS3_SPURS_LFQ_BYTES);
    be_put(out, 0x00, 2, 0);
    be_put(out, 0x10, 4, r->elem_size);
    be_put(out, 0x02, 2, 0);
    be_put(out, 0x14, 4, r->depth);
    be_put(out, 0x06, 2, 0);
    be_put(out, 0x18, 8, r->buffer);
    be_put(out, 0x08, 2, 0);
    be_put(out, 0x24, 4, r->direction);
    be_put(out, 0x0A, 2, 0);
    be_put(out, 0x0E, 2, 0);
    be_put(out, 0x30, 2, 0);
    be_put(out, 0x70, 8, r->owner);
    if (spurs_shape) {
        be_put(out, 0x18, 8, (uint64_t)r->buffer | 1u);
        be_put(out, 0x04, 2, 0);
        be_put(out, 0x21, 1, 0xFF);
        be_put(out, 0x50, 2, 0xFFFF);
        be_put(out, 0x0C, 2, 0);
        be_put(out, 0x20, 1, 0xFF);
        be_put(out, 0x28, 4, 0xFFFFFFFF);
        be_put(out, 0x30, 2, 0xFFFF);
        be_put(out, 0x78, 4, 0);
        be_put(out, 0x7C, 4, 0);
    } else {
        be_put(out, 0x28, 4, 0);
        be_put(out, 0x78, 8, 0);
        be_put(out, 0x20, 4, 0xFFFFFFFF);
    }
}

static void test_lfq_constants(void)
{
    /* CONTRACT §A.1, §A.3, §B.3, §D and the resolved export index. */
    CHECK_EQ_U64(PS3_SPURS_LFQ_BYTES, 128u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_ELEM_SIZE, 0x10u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_DEPTH, 0x14u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_BUFFER, 0x18u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_DIRECTION, 0x24u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_OWNER, 0x70u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_H8, 0x08u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_AUX_BASE, 0x30u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_AUX2_BASE, 0x50u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_TASKSET_VERSION, 0x24u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_SPURS_VERSION, 0x74u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_SPURS_VERSION_MAX, 0x1Fu);
    CHECK_EQ_U64(PS3_SPURS_LFQ_AUX_RUN_BYTES, 30u);
    /* g14b: the state word the dispatcher 0x209C reads at queue+0x2C. */
    CHECK_EQ_U64(PS3_SPURS_LFQ_STATE, 0x2Cu);
    CHECK_EQ_U64(PS3_SPURS_LFQ_STATE_INITIALIZED, 2u);
    /* The five codes, all in the TASK namespace the taskset block uses. */
    CHECK_EQ_U64(PS3_SPURS_LFQ_NULL, 0x80410911u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_ALIGN, 0x80410910u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_INVAL, 0x80410902u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_RETRY, 0x80410914u);
    CHECK_EQ_U64(PS3_SPURS_LFQ_STAT, 0x8041090Fu);
    /* None of them equals the CORE-namespace codes the probe speaks. */
    CHECK(PS3_SPURS_LFQ_INVAL != PS3_SPURS_LFQ_PROBE_INVAL);
    CHECK(PS3_SPURS_LFQ_STAT != PS3_SPURS_LFQ_PROBE_STAT);
    CHECK(PS3_SPURS_LFQ_NULL != PS3_SPURS_LFQ_PROBE_FULL);
}

static void test_lfq_owner_tag(void)
{
    /* The spurs arm is
     * the all-zero low nibble. inFAMOUS passes `taskset | 1`. */
    CHECK(ps3_spurs_lfq_owner_of(LFQ_GAME_TASKSET | 1u) == PS3_SPURS_LFQ_OWNER_TASKSET);
    CHECK(ps3_spurs_lfq_owner_of(LFQ_GAME_TASKSET) == PS3_SPURS_LFQ_OWNER_SPURS);
    CHECK(ps3_spurs_lfq_owner_of(LFQ_GAME_TASKSET | 3u) == PS3_SPURS_LFQ_OWNER_TASKSET);
    CHECK(ps3_spurs_lfq_owner_of(0u) == PS3_SPURS_LFQ_OWNER_SPURS);
    /* The stripped base is the taskset EA; the spurs arm hands r3 straight through. */
    CHECK_EQ_U64(ps3_spurs_lfq_owner_base(LFQ_GAME_TASKSET | 1u), LFQ_GAME_TASKSET);
    CHECK_EQ_U64(ps3_spurs_lfq_owner_base(LFQ_GAME_TASKSET), LFQ_GAME_TASKSET);
    /* The stored field keeps the tag (the low 32 bits of r3). */
    uint32_t field = 0;
    CHECK(ps3_spurs_lfq_owner_field(LFQ_GAME_TASKSET | 1u, &field) == 0);
    CHECK_EQ_U64(field, LFQ_GAME_TASKSET | 1u);
    CHECK(ps3_spurs_lfq_owner_field(LFQ_GAME_TASKSET | 1u, NULL) == -1);
}

static void test_lfq_gates(void)
{
    spurs_lfq_request r;
    uint32_t code = 0xDEADBEEF;

    /* inFAMOUS's own call passes every gate. */
    lfq_game_request(&r);
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_U64(code, 0u);

    /* Gate 1: queue == 0 -> 0x80410911 (the dispatcher's first test). The spurs arm's
     * dispatcher is a different entry, which never looks at the queue, so this
     * gate belongs to the taskset arm -- the arm inFAMOUS takes (r3 = taskset | 1). */
    lfq_game_request(&r); code = 0xDEADBEEF; r.queue = 0u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410911u);

    /* Gate 2: elem_size == 0 -> 0x80410911. The dispatcher's
     * r5 is the caller's r6. */
    lfq_game_request(&r); r.elem_size = 0u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410911u);

    /* Gates 3-7 all report 0x80410902: depth == 0 (0x20F4), elem_size > 0x4000 (0x217C),
     * elem_size & 0xF (0x2184), depth's HIGH HALF, direction > 3
     * (0x21A0). A depth of 9 is legal: the firmware bounds the high half, not the value. */
    lfq_game_request(&r); r.depth = 0u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);
    lfq_game_request(&r); r.depth = 9u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_OK);
    lfq_game_request(&r); r.elem_size = 0x4010u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);
    lfq_game_request(&r); r.elem_size = 0x28u;                 /* & 0xF != 0 */
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);
    lfq_game_request(&r); r.depth = 0x10003u;                  /* high half set */
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);
    lfq_game_request(&r); r.direction = 4u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);

    /* The exact boundaries that pass: elem_size 0x4000 with & 0xF == 0, depth 3, direction 3. */
    lfq_game_request(&r); r.elem_size = 0x4000u; r.depth = 3u; r.direction = 3u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_U64(code, 0u);
    /* And elem_size 0x10 with depth 1 is the smallest legal combination. */
    lfq_game_request(&r); r.elem_size = 0x10u; r.depth = 1u; r.direction = 0u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_OK);

    /* Gates 8 and 9: 0x80410910. The OWNER is masked with 0x7F (low 7 bits, tag
     * bit already dropped) and the BUFFER with 0xF. The QUEUE is
     * never alignment-checked. */
    lfq_game_request(&r); r.queue = LFQ_GAME_TASKSET + 0x8u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_OK);
    lfq_game_request(&r); r.buffer = LFQ_GAME_TASKSET + 0x84u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410910u);
    lfq_game_request(&r); r.owner = (LFQ_GAME_TASKSET | 1u) + 0x40u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410910u);
    /* Bit 7 of the owner is outside the 0x7F mask, so it passes. */
    lfq_game_request(&r); r.owner = (LFQ_GAME_TASKSET | 1u) + 0x80u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_OK);

    /* First failure wins: a NULL queue beats a NULL elem size, which beats a bad depth. */
    lfq_game_request(&r); r.queue = 0u; r.elem_size = 0u; r.depth = 0u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410911u);
    lfq_game_request(&r); r.elem_size = 0u; r.depth = 0u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410911u);
    /* The align codes come last: an unaligned buffer with a bad DIRECTION reports the direction's
     * 0x80410902 (gate 7), not the alignment (gate 9). */
    lfq_game_request(&r); r.buffer = LFQ_GAME_TASKSET + 0x84u; r.direction = 4u;
    CHECK(ps3_spurs_lfq_validate(&r, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);

    /* A refused call writes NO code and reports BAD_ARGUMENT. */
    CHECK(ps3_spurs_lfq_validate(NULL, &code) == PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    lfq_game_request(&r);
    CHECK(ps3_spurs_lfq_validate(&r, NULL) == PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
}

static void test_lfq_version_gate(void)
{
    spurs_lfq_request r;
    uint32_t code = 0xDEADBEEF;

    /* The taskset arm never gates its version: 0x17228 loads taskset+0x24 and 0x17234/0x17238
     * only compares it to 3. */
    lfq_game_request(&r);
    for (uint32_t version = 0; version < 8u; ++version) {
        code = 0xDEADBEEF;
        CHECK(ps3_spurs_lfq_version_gate(&r, version, &code) == PS3_SPURS_LFQ_STATUS_OK);
        CHECK_EQ_U64(code, 0u);
    }
    code = 0xDEADBEEF;
    CHECK(ps3_spurs_lfq_version_gate(&r, 0xFFFFFFFFu, &code) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_U64(code, 0u);

    /* The spurs arm refuses above 0x1F with 0x80410902. */
    lfq_game_request(&r); r.owner = LFQ_GAME_TASKSET;
    code = 0xDEADBEEF;
    CHECK(ps3_spurs_lfq_version_gate(&r, 0x1Fu, &code) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_U64(code, 0u);
    code = 0xDEADBEEF;
    CHECK(ps3_spurs_lfq_version_gate(&r, 0x20u, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);
    code = 0xDEADBEEF;
    CHECK(ps3_spurs_lfq_version_gate(&r, 0xFFFFFFFFu, &code) == PS3_SPURS_LFQ_STATUS_REFUSED);
    CHECK_EQ_U64(code, 0x80410902u);

    /* The shape predicate: true only for the taskset arm with version == 3. */
    lfq_game_request(&r);
    CHECK(ps3_spurs_lfq_uses_spurs_shape(&r, 3u) == 1);
    CHECK(ps3_spurs_lfq_uses_spurs_shape(&r, 2u) == 0);
    CHECK(ps3_spurs_lfq_uses_spurs_shape(&r, 0u) == 0);
    lfq_game_request(&r); r.owner = LFQ_GAME_TASKSET;
    CHECK(ps3_spurs_lfq_uses_spurs_shape(&r, 3u) == 0);   /* the spurs arm never reads +0x24 */
    CHECK(ps3_spurs_lfq_uses_spurs_shape(NULL, 3u) == 0);
}

static void test_lfq_infamous_image(void)
{
    spurs_lfq_request r;
    uint8_t expected[PS3_SPURS_LFQ_BYTES];
    guarded g;

    lfq_game_request(&r);
    /* The game's taskset carries version 0 (the g13 image leaves +0x24 zero), so the shape is the
     * CellSyncLFQueue one. */
    CHECK(ps3_spurs_lfq_uses_spurs_shape(&r, 0u) == 0);
    lfq_expected_line(expected, &r, 0);

    guarded_init(&g, PS3_SPURS_LFQ_BYTES);
    CHECK(ps3_spurs_lfq_image(&r, 0u, g.image, g.capacity) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_LFQ_BYTES);
    CHECK(guarded_intact(&g));

    /* Stated the way the contract words it, field by field. */
    CHECK_EQ_U64(be_get(g.image, 0x10, 4), 0x20u);
    CHECK_EQ_U64(be_get(g.image, 0x14, 4), 3u);
    CHECK_EQ_U64(be_get(g.image, 0x24, 4), 3u);
    {
        uint64_t buffer = 0, owner = 0;
        for (size_t i = 0; i < 8; ++i) buffer = buffer << 8 | g.image[0x18 + i];
        for (size_t i = 0; i < 8; ++i) owner = owner << 8 | g.image[0x70 + i];
        CHECK_EQ_U64(buffer, r.buffer);
        CHECK_EQ_U64(owner, LFQ_GAME_TASKSET | 1u);   /* the tag survives */
    }
    /* The CellSyncLFQueue tail writes exactly three fields outside the scalars: +0x28 (u32 0),
     * +0x78 (u64 0) and +0x20 (u32 0xFFFFFFFF, 0x2040/0x2048 -- a FOUR-byte store, so +0x21..+0x23
     * are 0xFF too, not the spurs shape's single +0x21 byte). It does NOT write the spurs shape's
     * +0x04, +0x0C, +0x30 or +0x50, which stay 0 only because the preamble is zeroed. */
    CHECK(g.image[0x04] == 0u && g.image[0x05] == 0u);
    CHECK(g.image[0x0C] == 0u && g.image[0x0D] == 0u);
    CHECK(g.image[0x30] == 0u && g.image[0x31] == 0u);
    CHECK(g.image[0x50] == 0u && g.image[0x51] == 0u);
    CHECK_EQ_U64(be_get(g.image, 0x20, 4), 0xFFFFFFFFu);
    CHECK_EQ_U64(be_get(g.image, 0x78, 8), 0u);

    /* A second call over a dirty buffer produces the same bytes: the builder's own writes are
     * the whole CellSyncLFQueue tail, and the zero-filled preamble is explicit. */
    memset(g.image, 0x5A, g.capacity);
    CHECK(ps3_spurs_lfq_image(&r, 0u, g.image, g.capacity) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_LFQ_BYTES);
    CHECK(guarded_intact(&g));
}

static void test_lfq_spurs_shape_image(void)
{
    spurs_lfq_request r;
    uint8_t expected[PS3_SPURS_LFQ_BYTES];
    guarded g;

    lfq_game_request(&r);
    lfq_expected_line(expected, &r, 1);
    guarded_init(&g, PS3_SPURS_LFQ_BYTES);
    CHECK(ps3_spurs_lfq_image(&r, 3u, g.image, g.capacity) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_MEM(g.image, expected, PS3_SPURS_LFQ_BYTES);
    CHECK(guarded_intact(&g));

    /* The eight CellSpursLFQueue-only fields, each read back. */
    CHECK(g.image[0x21] == 0xFFu);
    CHECK(g.image[0x20] == 0xFFu);
    CHECK_EQ_U64(be_get(g.image, 0x28, 4), 0xFFFFFFFFu);
    CHECK(g.image[0x30] == 0xFFu && g.image[0x31] == 0xFFu);
    CHECK(g.image[0x50] == 0xFFu && g.image[0x51] == 0xFFu);
    {
        uint64_t buffer = 0;
        for (size_t i = 0; i < 8; ++i) buffer = buffer << 8 | g.image[0x18 + i];
        CHECK_EQ_U64(buffer, (uint64_t)r.buffer | 1u);
    }

    /* The in-place form over the caller's own bytes: fields the firmware does not write keep
     * whatever the object already held. This is the shape the adapter uses. */
    {
        uint8_t io[PS3_SPURS_LFQ_BYTES];
        memset(io, 0x33, sizeof(io));
        CHECK(ps3_spurs_lfq_image_in_place(&r, 0u, io, sizeof(io)) == PS3_SPURS_LFQ_STATUS_OK);
        /* Written fields hold the same values as the zeroed image. The byte ranges below are
         * exactly the ones the firmware's stores address: 0x00..0x04, 0x06..0x0C, 0x0E..0x20,
         * 0x24..0x28, 0x28..0x2C, 0x20..0x24, 0x70..0x78 and 0x78..0x80. */
        lfq_expected_line(expected, &r, 0);
        for (size_t i = 0x00; i < 0x04; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x06; i < 0x0C; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x0E; i < 0x18; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x18; i < 0x20; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x24; i < 0x28; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x70; i < 0x78; ++i) CHECK_EQ_U64(io[i], expected[i]);
        /* ... plus the three tail fields ... */
        for (size_t i = 0x28; i < 0x2C; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x20; i < 0x24; ++i) CHECK_EQ_U64(io[i], expected[i]);
        for (size_t i = 0x78; i < 0x80; ++i) CHECK_EQ_U64(io[i], expected[i]);
        /* ... and the bytes neither the line's stores nor the tail touches keep the caller's
         * 0x33: +0x04..0x06, +0x0C..0x0E, +0x21, +0x30..0x32, +0x4C..0x50. The auxiliary runs are
         * separate writes into the object, not part of this 128-byte call. */
        for (size_t i = 0x4C; i < 0x50; ++i) CHECK_EQ_U64(io[i], 0x33u);
        CHECK_EQ_U64(io[0x04], 0x33u);
        CHECK_EQ_U64(io[0x0C], 0x33u);
        /* +0x30 IS written, by the scalar preamble's first 2-byte store at queue+0x30. */
        CHECK_EQ_U64(io[0x30], 0x00u);
        CHECK_EQ_U64(io[0x31], 0x00u);
        CHECK_EQ_U64(io[0x50], 0x33u);
        /* +0x20..+0x23 is one FOUR-byte store of 0xFFFFFFFF (0x2048), so +0x21..+0x23 are 0xFF. */
        for (size_t i = 0x20; i < 0x24; ++i) CHECK_EQ_U64(io[i], 0xFFu);
    }
}

static void test_lfq_bad_arguments(void)
{
    spurs_lfq_request r;
    uint8_t small[PS3_SPURS_LFQ_BYTES - 1u];
    uint8_t aux[PS3_SPURS_LFQ_AUX_RUN_BYTES];

    lfq_game_request(&r);
    CHECK(ps3_spurs_lfq_image(&r, 0u, NULL, PS3_SPURS_LFQ_BYTES) ==
          PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_image(&r, 0u, small, sizeof(small)) ==
          PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_image(NULL, 0u, small, sizeof(small)) ==
          PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_image_in_place(&r, 0u, NULL, PS3_SPURS_LFQ_BYTES) ==
          PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_image_in_place(&r, 0u, small, sizeof(small)) ==
          PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_aux_image(0u, NULL, sizeof(aux)) == PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_aux_image(0u, aux, sizeof(aux) - 1u) ==
          PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_aux_image(2u, aux, sizeof(aux)) == PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_state_image(NULL, 2u) == PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_state_image(small, 1u) == PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT);
    CHECK(ps3_spurs_lfq_aux_address(r.queue, 0u, NULL) == -1);
    CHECK(ps3_spurs_lfq_aux_address(r.queue, 2u, &r.depth) == -1);
    CHECK(ps3_spurs_lfq_put_be(NULL, 4u, 0u, 4u, 1u) == -1);
    CHECK(ps3_spurs_lfq_put_be(small, sizeof(small), 0u, 3u, 1u) == -1);
    CHECK(ps3_spurs_lfq_put_be(small, sizeof(small), sizeof(small), 1u, 1u) == -1);
}

static void test_lfq_auxiliary_runs(void)
{
    guarded g;
    uint32_t address = 0;

    /* 0x1FE4..0x2014: two 15-iteration loops at stride 2 from queue+0x30 and queue+0x50. */
    guarded_init(&g, PS3_SPURS_LFQ_AUX_RUN_BYTES);
    memset(g.image, 0xFF, PS3_SPURS_LFQ_AUX_RUN_BYTES);
    CHECK(ps3_spurs_lfq_aux_image(0u, g.image, g.capacity) == PS3_SPURS_LFQ_STATUS_OK);
    check_all_zero(g.image, PS3_SPURS_LFQ_AUX_RUN_BYTES);
    CHECK(guarded_intact(&g));

    memset(g.image, 0xFF, PS3_SPURS_LFQ_AUX_RUN_BYTES);
    CHECK(ps3_spurs_lfq_aux_image(1u, g.image, g.capacity) == PS3_SPURS_LFQ_STATUS_OK);
    check_all_zero(g.image, PS3_SPURS_LFQ_AUX_RUN_BYTES);
    CHECK(guarded_intact(&g));

    /* The guest addresses: 0x30 and 0x50, i.e. 30 bytes each, leaving 0x4C..0x50 unwritten. */
    CHECK(ps3_spurs_lfq_aux_address(LFQ_GAME_TASKSET, 0u, &address) == 0);
    CHECK_EQ_U64(address, LFQ_GAME_TASKSET + 0x30u);
    CHECK(ps3_spurs_lfq_aux_address(LFQ_GAME_TASKSET, 1u, &address) == 0);
    CHECK_EQ_U64(address, LFQ_GAME_TASKSET + 0x50u);
    /* The two runs plus the 4-byte hole exactly cover 0x30..0x70 of the line. */
    CHECK_EQ_U64(0x30u + PS3_SPURS_LFQ_AUX_RUN_BYTES, 0x4Eu);
    CHECK_EQ_U64(0x50u + PS3_SPURS_LFQ_AUX_RUN_BYTES, 0x6Eu);
    CHECK_EQ_U64(PS3_SPURS_LFQ_OWNER, 0x6Eu + 2u);
}

static void test_lfq_collapse_and_state(void)
{
    uint8_t state[2];

    /* `(code & 0xFFFFFF00) | 0x80410900`. */
    /* The two remaps fire on the WHOLE 32-bit code, so only the task
     * namespace's spellings are caught; the probe's own core-namespace codes are not. */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x80410902u), 0x80410914u);   /* remapped to 0x914 */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x8041090Fu), 0x80410914u);   /* remapped to 0x914 */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x80410109u), 0x80410909u);   /* folded only          */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x8041010Fu), 0x8041090Fu);   /* folded only          */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x80410101u), 0x80410901u);   /* folded only          */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x80410660u), 0x80410960u);   /* folded only          */
    /* Only the low byte survives, and only for a NEGATIVE code. */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x80410113u), 0x80410913u);
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0xDEADBEEFu), 0x804109EFu);
    /* A NON-negative code is returned unchanged (a SIGNED compare); the
     * probe's sc-130 pass-through has bit 31 set and so IS folded. */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x80010000u), 0x80410900u);
    /* ... but a small positive value is. */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0x00000123u), 0x00000123u);
    /* Zero is non-negative and is returned as it stands -- which is the success value. */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(0u), 0u);
    /* The two remaps both land on the SAME word, so they cannot be told apart. */
    CHECK_EQ_U64(ps3_spurs_lfq_collapse(PS3_SPURS_LFQ_INVAL),
                 ps3_spurs_lfq_collapse(PS3_SPURS_LFQ_STAT));

    /* g14b: the entry's RESULT comes from the object's state word, not from a probe.
     *
     * 0x220C..0x2248 has already probed queue+0x2C; 0x226C/0x2270 send a word of 2 to 0x22E4 and
     * everything else to the builder at 0x2274, which ends with r31 == 0. On
     * inFAMOUS's arguments (+0x2C is 0) the answer is therefore 0. */
    {
        spurs_lfq_request r;
        uint8_t line[PS3_SPURS_LFQ_BYTES];
        lfq_game_request(&r);
        /* The line the builder writes on the game's own shape. Shape 0 is the one whose +0x18 keeps
         * the untouched buffer EA, which is what the compare then matches. */
        lfq_expected_line(line, &r, 0);
        CHECK_EQ_U64(ps3_spurs_lfq_state_of(0u), PS3_SPURS_LFQ_STATE_REBUILD);
        CHECK_EQ_U64(ps3_spurs_lfq_state_of(2u), PS3_SPURS_LFQ_STATE_MATCH);
        CHECK_EQ_U64(ps3_spurs_lfq_state_of(1u), PS3_SPURS_LFQ_STATE_REBUILD);
        /* An uninitialized object: 0, on either shape. */
        line[PS3_SPURS_LFQ_STATE] = line[PS3_SPURS_LFQ_STATE + 1] = 0;
        line[PS3_SPURS_LFQ_STATE + 2] = line[PS3_SPURS_LFQ_STATE + 3] = 0;
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, line, sizeof(line)), 0u);
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 3u, line, sizeof(line)), 0u);
        /* An initialized object (0x2298 stored 2) whose line agrees: 0x22E4 accepts it. */
        be_put(line, PS3_SPURS_LFQ_STATE, 4, 2u);
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, line, sizeof(line)), 0u);
        CHECK_EQ_U64(ps3_spurs_lfq_match(&r, line, sizeof(line)), 0u);
        /* ... and one whose line does not: the shared 0x80410902 word. */
        r.depth = 4u;
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, line, sizeof(line)), 0x80410902u);
        CHECK_EQ_U64(ps3_spurs_lfq_match(&r, line, sizeof(line)), 0x80410902u);
        r.depth = 3u;
        line[PS3_SPURS_LFQ_ELEM_SIZE + 3] ^= 1u;
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, line, sizeof(line)), 0x80410902u);
        line[PS3_SPURS_LFQ_ELEM_SIZE + 3] ^= 1u;
        line[PS3_SPURS_LFQ_DIRECTION] = 2u;
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, line, sizeof(line)), 0x80410902u);
        line[PS3_SPURS_LFQ_DIRECTION] = 0u;
        /* Bad arguments. */
        CHECK_EQ_U64(ps3_spurs_lfq_result(NULL, 0u, line, sizeof(line)), PS3_SPURS_LFQ_INVAL);
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, NULL, sizeof(line)), PS3_SPURS_LFQ_INVAL);
        CHECK_EQ_U64(ps3_spurs_lfq_result(&r, 0u, line, 8u), PS3_SPURS_LFQ_INVAL);
        CHECK_EQ_U64(ps3_spurs_lfq_match(NULL, line, sizeof(line)), PS3_SPURS_LFQ_INVAL);
    }

    /* g14's probe reading is kept, but as `ps3_spurs_lfq_probe_result`: a push attempt's answer for
     * the SETTER, which is a different export. Nothing on this entry's path calls it. */
    {
        spurs_lfq_request r;
        lfq_game_request(&r);
        CHECK_EQ_U64(ps3_spurs_lfq_probe_result(&r, 0u), 0x80410909u);
        r.direction = 2u;
        CHECK_EQ_U64(ps3_spurs_lfq_probe_result(&r, 0u), 0u);
        r.direction = 1u; r.elem_size = 3u;
        CHECK_EQ_U64(ps3_spurs_lfq_probe_result(&r, 0u), 0u);
        r.direction = 2u; r.elem_size = 3u;
        CHECK_EQ_U64(ps3_spurs_lfq_probe_result(&r, 0u), 0x80410909u);
        r.direction = 3u; r.elem_size = 0x20u;
        CHECK_EQ_U64(ps3_spurs_lfq_probe_result(&r, 3u), 0u);
        CHECK_EQ_U64(ps3_spurs_lfq_probe_result(NULL, 0u), PS3_SPURS_LFQ_PROBE_REFUSED);
    }

    /* The state word the entry leaves at queue+0x08 is zero on both shapes. */
    memset(state, 0xFF, sizeof(state));
    CHECK(ps3_spurs_lfq_state_image(state, sizeof(state)) == PS3_SPURS_LFQ_STATUS_OK);
    CHECK_EQ_U64(state[0], 0u);
    CHECK_EQ_U64(state[1], 0u);
}

/* ------------------------------------------------------------------------------------ */

int main(void)
{
    test_sizes();
    test_sign_extend();
    test_put_be();

    test_attribute_infamous_image();
    test_attribute_copies_priority_verbatim();
    test_attribute_no_revision_branch();
    test_attribute_null();
    test_attribute_alignment();
    test_attribute_priority_range();
    test_attribute_validation_order();
    test_attribute_bad_arguments();

    test_event_flag_infamous_image();
    test_event_flag_taskset_branch();
    test_event_flag_boundaries();
    test_event_flag_null();
    test_event_flag_alignment();
    test_event_flag_ranges();
    test_event_flag_validation_order();
    test_event_flag_bad_arguments();

    test_attribute_set_name();
    test_event_flag_attach();

    test_idempotent();

    test_create_validate();
    test_create_arguments();
    test_create_image();
    test_create_wid_and_flag_images();
    test_create_flag_store();
    test_create_registration();
    test_create_workload_attribute_image();
    test_create_collapse();
    test_create_constants();

    test_lfq_constants();
    test_lfq_owner_tag();
    test_lfq_gates();
    test_lfq_version_gate();
    test_lfq_infamous_image();
    test_lfq_spurs_shape_image();
    test_lfq_bad_arguments();
    test_lfq_auxiliary_runs();
    test_lfq_collapse_and_state();

    if (failures) {
        fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}

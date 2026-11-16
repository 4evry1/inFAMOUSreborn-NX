/* Pure domain tests for _cellSpursWorkloadAttributeInitialize / SetName (spurs_attributes.{h,c}).
 * Expected bytes are INDEPENDENT literals, never produced by the encoder under test. */
#include "spurs_attributes.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_U(actual, expected) do { const unsigned long long a_ = (unsigned long long)(actual), \
    e_ = (unsigned long long)(expected); ++checks; if (a_ != e_) { ++failures; \
    fprintf(stderr, "FAIL %s:%d: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #actual, a_, e_); } } while (0)

_Static_assert(PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE == 512, "size");
_Static_assert(PS3_SPURS_WORKLOAD_ATTRIBUTE_ALIGNMENT == 8, "align");
_Static_assert(PS3_SPURS_WORKLOAD_POLICY_MODULE_ALIGNMENT == 16, "pm align");
_Static_assert(PS3_SPURS_WORKLOAD_PRIORITY_COUNT == 8, "priority");
_Static_assert(PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_CLASS_OFFSET == 0x28, "class off");
_Static_assert(PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_INSTANCE_OFFSET == 0x2c, "instance off");
_Static_assert(PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE == 8, "patch size");

enum { OK = PS3_SPURS_WORKLOAD_ATTRIBUTE_OK, HOST = PS3_SPURS_WORKLOAD_ATTRIBUTE_INVALID_HOST_ARGUMENT };
#define IMG PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE
#define ST(x) ((ps3_spurs_workload_attribute_status)(x))

/* Independent literal: revision 1, sdk 0x220002, pm 0x69c880, size 0x2d80, data 0x404e9300,
 * priority 00 01 02 0F 08 00 0E 07, min 1, max 5, then 472 zero bytes (static storage). */
static const unsigned char expected_head[0x28] = {
    0x00, 0x00, 0x00, 0x01,  0x00, 0x22, 0x00, 0x02,  0x00, 0x69, 0xc8, 0x80,  0x00, 0x00, 0x2d, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x4e, 0x93, 0x00,
    0x00, 0x01, 0x02, 0x0f, 0x08, 0x00, 0x0e, 0x07,
    0x00, 0x00, 0x00, 0x01,  0x00, 0x00, 0x00, 0x05 };
static unsigned char expected_image[IMG];

static ps3_spurs_workload_attribute_request actual_like(void)
{
    static const unsigned char prio[8] = { 0x00, 0x01, 0x02, 0x0f, 0x08, 0x00, 0x0e, 0x07 };
    ps3_spurs_workload_attribute_request r;
    memset(&r, 0, sizeof r);
    r.attribute = 0x1000; r.revision = 1; r.sdk_version = 0x220002; r.policy_module = 0x69c880;
    r.size = 0x2d80; r.data = UINT64_C(0x404e9300); memcpy(r.priority, prio, 8);
    r.min_contention = 1; r.max_contention = 5;
    return r;
}

static void test_literal(void)
{
    memcpy(expected_image, expected_head, sizeof expected_head);
    unsigned char image[IMG];
    memset(image, 0xA5, sizeof image);
    const ps3_spurs_workload_attribute_request r = actual_like();
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    CHECK(memcmp(image, expected_image, IMG) == 0);
    for (unsigned i = 0x28; i < IMG; ++i) CHECK(image[i] == 0);
    /* full 64-bit data kept, big-endian */
    ps3_spurs_workload_attribute_request w = actual_like();
    w.data = UINT64_C(0x0123456789abcdef);
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&w, image), OK);
    static const unsigned char d[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    CHECK(memcmp(image + 0x10, d, 8) == 0);
}

static void test_status_values(void)
{
    CHECK_EQ_U(PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL, 0x80410811u);
    CHECK_EQ_U(PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN, 0x80410810u);
    CHECK_EQ_U(PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL, 0x80410802u);
    CHECK_EQ_U(PS3_SPURS_WORKLOAD_ATTRIBUTE_OK, 0);
    CHECK_EQ_U(PS3_SPURS_WORKLOAD_ATTRIBUTE_INVALID_HOST_ARGUMENT, 1);
    /* distinct from the instance attribute 804107xx codes */
    CHECK((unsigned)PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL != (unsigned)PS3_SPURS_ATTRIBUTE_NULL);
}

static void expect_untouched(const ps3_spurs_workload_attribute_request *r, unsigned expected_status)
{
    unsigned char image[IMG], before[IMG];
    memset(image, 0xA5, sizeof image); memcpy(before, image, sizeof image);
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(r, image), expected_status);
    CHECK(memcmp(image, before, IMG) == 0);
}

static void test_pointer_errors_untouched(void)
{
    ps3_spurs_workload_attribute_request r = actual_like();
    r.attribute = 0; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    for (uint32_t a = 1; a < 8; ++a) { r = actual_like(); r.attribute = 0x1000 + a;
        expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN); }
    r = actual_like(); r.attribute = 0x1004; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    r = actual_like(); r.policy_module = 0; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    r = actual_like(); r.policy_module = 8; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    r = actual_like(); r.policy_module = 0x69c881; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    /* pm only needs 16, not just 8: 0x10 passes, 0x18 fails */
    r = actual_like(); r.policy_module = 0x10;
    { unsigned char image[IMG]; CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK); }
    r = actual_like(); r.policy_module = 0x18; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
}

static void test_contention_errors_untouched(void)
{
    ps3_spurs_workload_attribute_request r = actual_like();
    r.min_contention = 0; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL);
    for (unsigned i = 0; i < 8; ++i) {
        static const unsigned char bad[] = { 0x10, 0x20, 0x40, 0x80, 0xF0, 0xFF, 0x1F };
        for (unsigned k = 0; k < sizeof bad; ++k) {
            r = actual_like(); r.priority[i] = bad[k];
            expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL);
        }
        /* low nibble 0x0F alone passes */
        r = actual_like(); memset(r.priority, 0, 8); r.priority[i] = 0x0F;
        unsigned char image[IMG]; CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
        CHECK(image[0x18 + i] == 0x0F);
    }
}

static void test_precedence(void)
{
    ps3_spurs_workload_attribute_request r = actual_like();
    r.attribute = 0; r.policy_module = 0; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    r.attribute = 0x1001; r.policy_module = 0; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN); /* attr align beats pm null */
    r.attribute = 0; r.policy_module = 8; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);     /* attr null beats pm align */
    r.attribute = 0x1001; r.policy_module = 8; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    r = actual_like(); r.policy_module = 0; r.min_contention = 0; r.priority[0] = 0xF0;
    expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);     /* pointer errors beat contention */
    r.policy_module = 8; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    r.attribute = 0; expect_untouched(&r, PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(0, 0), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(4, 0), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(0, 8), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(8, 0), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(8, 4), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(8, 16), OK);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_pointers(0xFFFFFFF8u, 0xFFFFFFF0u), OK); /* numeric only */
}

static void test_validate_contention(void)
{
    static const unsigned char good[8] = { 0, 1, 2, 0x0F, 8, 0, 0x0E, 7 };
    unsigned char bad[8]; memcpy(bad, good, 8); bad[7] = 0x80;
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_contention(1, good), OK);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_contention(0xFFFFFFFFu, good), OK);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_contention(0, good), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_contention(1, bad), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_contention(1, NULL), HOST);
    CHECK_EQ_U(ps3_spurs_workload_attribute_validate_contention(0, NULL), HOST); /* host error, not INVAL */
}

static void test_accepted_unusual(void)
{
    unsigned char image[IMG];
    ps3_spurs_workload_attribute_request r = actual_like();
    r.revision = 3; r.sdk_version = 0; r.size = 0; r.max_contention = 0;
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    CHECK_EQ_U(image[3], 3); CHECK_EQ_U(image[7], 0); CHECK_EQ_U(image[15], 0);
    CHECK(image[0x24] == 0 && image[0x25] == 0 && image[0x26] == 0 && image[0x27] == 0);
    r = actual_like(); r.revision = 0xFFFFFFFFu; r.sdk_version = 0xFFFFFFFFu; r.size = 0xFFFFFFFFu;
    r.max_contention = 0xFFFFFFFFu; r.min_contention = 0xFFFFFFFFu;
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    for (unsigned i = 0; i < 4; ++i) { CHECK(image[i] == 0xFF); CHECK(image[4 + i] == 0xFF);
        CHECK(image[12 + i] == 0xFF); CHECK(image[0x20 + i] == 0xFF); CHECK(image[0x24 + i] == 0xFF); }
    CHECK(image[8] == 0x00 && image[9] == 0x69); /* pm unchanged */
    r = actual_like(); r.min_contention = 7; r.max_contention = 2; /* min > max accepted */
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    CHECK_EQ_U(image[0x23], 7); CHECK_EQ_U(image[0x27], 2);
    r = actual_like(); r.revision = 0x01020304u; r.sdk_version = 0x05060708u; r.size = 0x090a0b0cu;
    r.max_contention = 0x0d0e0f10u;
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    static const unsigned char be[] = { 1,2,3,4, 5,6,7,8 };
    CHECK(memcmp(image, be, 8) == 0);
    CHECK(image[12] == 9 && image[13] == 10 && image[14] == 11 && image[15] == 12);
    CHECK(image[0x24] == 0x0d && image[0x25] == 0x0e && image[0x26] == 0x0f && image[0x27] == 0x10);
}

static void test_host_arguments(void)
{
    unsigned char image[IMG], before[IMG];
    memset(image, 0xA5, sizeof image); memcpy(before, image, sizeof image);
    const ps3_spurs_workload_attribute_request r = actual_like();
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(NULL, image), HOST);
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, NULL), HOST);
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(NULL, NULL), HOST);
    CHECK(memcmp(image, before, IMG) == 0);
    /* HOST comes before SDK validation even when the request would be a guest error */
    ps3_spurs_workload_attribute_request bad = actual_like(); bad.attribute = 0;
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&bad, NULL), HOST);
}

static void test_request_alias_snapshot(void)
{
    /* The request lives INSIDE the destination image: zeroing must not destroy inputs. */
    union { ps3_spurs_workload_attribute_request request; unsigned char image[IMG]; } u;
    memset(&u, 0x5A, sizeof u);
    u.request = actual_like();
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&u.request, u.image), OK);
    CHECK(memcmp(u.image, expected_image, IMG) == 0);
    /* An error through the alias leaves the aliased storage (request included) untouched. */
    union { ps3_spurs_workload_attribute_request request; unsigned char image[IMG]; } e, snap;
    memset(&e, 0x5A, sizeof e);
    e.request = actual_like(); e.request.min_contention = 0;
    memcpy(&snap, &e, sizeof e);
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&e.request, e.image), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL);
    CHECK(memcmp(&e, &snap, sizeof e) == 0);
    /* Input request is never modified on success. */
    const ps3_spurs_workload_attribute_request orig = actual_like();
    ps3_spurs_workload_attribute_request copy = orig; unsigned char image[IMG];
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&copy, image), OK);
    CHECK(memcmp(&copy, &orig, sizeof copy) == 0);
}

static void test_build_idempotent_and_clears_previous(void)
{
    unsigned char image[IMG]; memset(image, 0xFF, sizeof image);
    const ps3_spurs_workload_attribute_request r = actual_like();
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    CHECK_EQ_U(ps3_spurs_workload_attribute_build(&r, image), OK);
    CHECK(memcmp(image, expected_image, IMG) == 0); /* whole 512 bytes overwritten, incl. tail */
}

static void expect_patch(uint32_t attr, uint32_t c, uint32_t i, const unsigned char want[8])
{
    unsigned char buffer[16]; memset(buffer, 0xA5, sizeof buffer);
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(attr, c, i, buffer), OK);
    CHECK(memcmp(buffer, want, 8) == 0);
    for (unsigned k = 8; k < 16; ++k) CHECK(buffer[k] == 0xA5); /* exactly 8 bytes written */
}

static void test_name_patch(void)
{
    static const unsigned char normal[8] = { 0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD };
    expect_patch(0x1000, 0x11223344u, 0xAABBCCDDu, normal);          /* class first, instance second */
    static const unsigned char zero[8] = { 0 };
    expect_patch(0x1000, 0, 0, zero);                                /* NULL names */
    static const unsigned char eq[8] = { 0x00, 0xd0, 0x01, 0x28, 0x00, 0xd0, 0x01, 0x28 };
    expect_patch(0x1000, 0x00d00128u, 0x00d00128u, eq);              /* equal names */
    static const unsigned char unm[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD, 0xBE, 0xEF };
    expect_patch(0x1000, 0xFFFFFFFFu, 0xDEADBEEFu, unm);             /* unmapped/odd/unaligned numbers */
    static const unsigned char inside[8] = { 0x00, 0x00, 0x10, 0x28, 0x00, 0x00, 0x10, 0x2c };
    expect_patch(0x1000, 0x1028, 0x102c, inside);                    /* names inside the attribute itself */
    expect_patch(0x8, 0x11223344u, 0xAABBCCDDu, normal);             /* low aligned attribute ok */
    expect_patch(0xFFFFFFF8u, 0x11223344u, 0xAABBCCDDu, normal);     /* wrap is an adapter concern; pure encodes */
    unsigned char buffer[8], before[8];
    memset(buffer, 0xA5, 8); memcpy(before, buffer, 8);
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0, 1, 2, buffer), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    CHECK(memcmp(buffer, before, 8) == 0);
    for (uint32_t a = 1; a < 8; ++a) {
        CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0x1000 + a, 1, 2, buffer), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
        CHECK(memcmp(buffer, before, 8) == 0);
    }
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0x1004, 1, 2, buffer), PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0x1010, 1, 2, buffer), OK);  /* only 8-alignment required */
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0x1008, 1, 2, buffer), OK);
    /* NULL patch is HOST and precedes the attribute policy checks */
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0x1000, 1, 2, NULL), HOST);
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0, 1, 2, NULL), HOST);
    CHECK_EQ_U(ps3_spurs_workload_attribute_name_patch(0x1001, 1, 2, NULL), HOST);
    /* patch offsets as published */
    CHECK_EQ_U(PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_INSTANCE_OFFSET - PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_CLASS_OFFSET, 4);
}

int main(void)
{
    test_literal();
    test_status_values();
    test_pointer_errors_untouched();
    test_contention_errors_untouched();
    test_precedence();
    test_validate_contention();
    test_accepted_unusual();
    test_host_arguments();
    test_request_alias_snapshot();
    test_build_idempotent_and_clears_previous();
    test_name_patch();
    printf("spurs workload attribute domain: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

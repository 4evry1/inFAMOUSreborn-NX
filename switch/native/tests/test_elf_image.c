#include "elf_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned char bytes[512]; int fault; } Fixture;
static int tests;
static void put(unsigned char *p, uint64_t v, unsigned n) {
    while (n) { p[--n] = (unsigned char)v; v >>= 8; }
}
static ElfReadStatus read_fixture(void *context, uint64_t offset, void *out, size_t n) {
    Fixture *f = context;
    if (f->fault == 1 && offset >= 64) return ELF_READ_ERROR;
    if (f->fault == 2 && offset >= 64) return ELF_READ_TRUNCATED;
    if (f->fault == 3 && offset >= 120) return ELF_READ_ERROR;
    if (f->fault == 4 && offset >= 320) return ELF_READ_ERROR;
    if (f->fault == 5 && offset >= 320) return ELF_READ_TRUNCATED;
    if (f->fault == 6 && offset >= 320) { memset(out,0x7a,n); return ELF_READ_ERROR; }
    if (offset > sizeof(f->bytes) || n > sizeof(f->bytes) - offset) return ELF_READ_TRUNCATED;
    memcpy(out, f->bytes + offset, n); return ELF_READ_OK;
}
static Fixture fixture(void) {
    Fixture f = {{0}, 0}; unsigned char *b = f.bytes;
    memcpy(b, "\177ELF", 4); b[4]=2; b[5]=2; b[6]=1;
    put(b+16,2,2); put(b+18,21,2); put(b+20,1,4);
    put(b+24,0x10000,8); put(b+32,64,8); put(b+52,64,2);
    put(b+54,56,2); put(b+56,2,2);
    put(b+64,1,4); put(b+68,5,4); put(b+72,192,8);
    put(b+80,0x10000,8); put(b+96,16,8); put(b+104,32,8); put(b+112,16,8);
    put(b+120,7,4); put(b+128,200,8); put(b+136,0x10008,8);
    put(b+152,8,8); put(b+160,16,8); put(b+168,8,8);
    return f;
}
static ElfImagePlan expect(Fixture *f, uint64_t size, size_t capacity, ElfImageStatus expected) {
    ElfImageSegment segments[2], before[2]; ElfImagePlan plan, old;
    memset(segments,0xa5,sizeof segments); memcpy(before,segments,sizeof segments);
    memset(&plan,0x5a,sizeof plan); old=plan;
    ElfImageReader r = {read_fixture,f,size};
    assert(elf_image_plan(&r,segments,capacity,&plan)==expected);
    if (expected != ELF_IMAGE_OK) {
        assert(memcmp(&plan,&old,sizeof plan)==0);
        assert(memcmp(segments,before,sizeof segments)==0);
    } else {
        assert(plan.entry_opd==0x10000 && plan.segment_count==1 && plan.has_tls);
        assert(plan.tls.guest_address==0x10008 && plan.tls.file_size==8 && plan.tls.memory_size==16);
        assert(segments[0].file_offset==192 && segments[0].guest_address==0x10000);
        assert(segments[0].file_size==16 && segments[0].memory_size==32);
        assert(segments[0].flags==5 && segments[0].alignment==16);
    }
    ++tests;
    return plan;
}

static Fixture process_fixture(void) {
    Fixture f = fixture();
    put(f.bytes + 56, 3, 2);
    unsigned char *ph = f.bytes + 176;
    put(ph, 0x60000001, 4);
    put(ph + 8, 320, 8);
    put(ph + 16, 0x10140, 8);
    put(ph + 32, 32, 8);
    put(ph + 40, 32, 8);
    put(ph + 48, 4, 8);
    uint32_t words[8] = {32, 0x13bcc5f6, 0x9000, 0x250001, 250, 0x40000, 0x100000, 0};
    for (unsigned index = 0; index < 8; ++index) put(f.bytes + 320 + index * 4, words[index], 4);
    return f;
}

static void test_process_parameters(void) {
    Fixture f = process_fixture();
    ElfImagePlan plan = expect(&f, 512, 2, ELF_IMAGE_OK);
    assert(plan.has_process_parameters);
    assert(plan.process_parameters.size == 32 && plan.process_parameters.magic == 0x13bcc5f6);
    assert(plan.process_parameters.version == 0x9000 && plan.process_parameters.sdk_version == 0x250001);
    assert(plan.process_parameters.primary_priority == 250 && plan.process_parameters.primary_stack_size == 0x40000);
    assert(plan.process_parameters.malloc_page_size == 0x100000 && plan.process_parameters.ppc_segment == 0);
    f = fixture(); plan = expect(&f, 256, 2, ELF_IMAGE_OK);
    ElfImageProcessParameters empty = {0};
    assert(!plan.has_process_parameters && !memcmp(&plan.process_parameters, &empty, sizeof(empty)));
    f = process_fixture(); put(f.bytes + 216, 64, 8); expect(&f, 512, 2, ELF_IMAGE_OK);
    f = process_fixture(); put(f.bytes + 216, 31, 8); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 224, 3, 8); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); expect(&f, 351, 2, ELF_IMAGE_TRUNCATED);
    f = process_fixture(); put(f.bytes + 320, 31, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 320, 36, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 324, 0, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 332, UINT32_MAX, 4); expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
    f = process_fixture(); put(f.bytes + 328, 0x12345678, 4); expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
    f = process_fixture(); put(f.bytes + 56, 4, 2); memcpy(f.bytes + 232, f.bytes + 176, 56);
    expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 208, 0, 8); put(f.bytes + 216, 0, 8); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 208, 36, 8); put(f.bytes + 216, 36, 8);
    expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
    f = process_fixture(); put(f.bytes + 208, 31, 8); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 184, UINT64_MAX, 8); expect(&f, 512, 2, ELF_IMAGE_TRUNCATED);
    f = process_fixture(); put(f.bytes + 192, UINT64_C(0x100000000), 8); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); f.fault = 4; expect(&f, 512, 2, ELF_IMAGE_READ_FAILURE);
    f = process_fixture(); f.fault = 5; expect(&f, 512, 2, ELF_IMAGE_TRUNCATED);
    f = process_fixture(); f.fault = 6; expect(&f, 512, 2, ELF_IMAGE_READ_FAILURE);
    const uint32_t versions[] = {1, 0x8400, 0x9000, 0x330000};
    for (unsigned index = 0; index < sizeof(versions)/sizeof(versions[0]); ++index) {
        f = process_fixture(); put(f.bytes + 328, versions[index], 4); expect(&f, 512, 2, ELF_IMAGE_OK);
    }
    const int32_t priorities[] = {-512, -1, 0, 3071};
    for (unsigned index = 0; index < sizeof(priorities)/sizeof(priorities[0]); ++index) {
        f = process_fixture(); put(f.bytes + 336, (uint32_t)priorities[index], 4);
        plan = expect(&f, 512, 2, ELF_IMAGE_OK);
        assert(plan.process_parameters.primary_priority == priorities[index]);
    }
    f = process_fixture(); put(f.bytes + 336, (uint32_t)-513, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 336, 3072, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 336, (uint32_t)-32768, 4); expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
    const uint32_t stacks[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x1000, 0x100000};
    for (unsigned index = 0; index < sizeof(stacks)/sizeof(stacks[0]); ++index) {
        f = process_fixture(); put(f.bytes + 340, stacks[index], 4);
        plan = expect(&f, 512, 2, ELF_IMAGE_OK);
        assert(plan.process_parameters.primary_stack_size == stacks[index]);
    }
    f = process_fixture(); put(f.bytes + 340, UINT32_MAX, 4); expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
    f = process_fixture(); put(f.bytes + 340, 0x1001, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 340, 0x80, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    f = process_fixture(); put(f.bytes + 340, 0x101000, 4); expect(&f, 512, 2, ELF_IMAGE_MALFORMED);
    const uint32_t pages[] = {0, 0x10000, 0x100000};
    for (unsigned index = 0; index < sizeof(pages)/sizeof(pages[0]); ++index) {
        f = process_fixture(); put(f.bytes + 344, pages[index], 4); expect(&f, 512, 2, ELF_IMAGE_OK);
    }
    f = process_fixture(); put(f.bytes + 344, 4096, 4); expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
    for (uint32_t segment = 0; segment <= 2; ++segment) {
        f = process_fixture(); put(f.bytes + 348, segment, 4); expect(&f, 512, 2, ELF_IMAGE_OK);
    }
    f = process_fixture(); put(f.bytes + 348, 3, 4); expect(&f, 512, 2, ELF_IMAGE_UNSUPPORTED);
}

int test_elf_image(void) {
    Fixture f=fixture(); expect(&f,256,2,ELF_IMAGE_OK);
    f=fixture(); expect(&f,63,2,ELF_IMAGE_TRUNCATED);
    f=fixture(); f.bytes[0]=0; expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); f.bytes[4]=1; expect(&f,256,2,ELF_IMAGE_UNSUPPORTED);
    f=fixture(); put(f.bytes+18,62,2); expect(&f,256,2,ELF_IMAGE_UNSUPPORTED);
    f=fixture(); put(f.bytes+54,55,2); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+32,UINT64_MAX-8,8); expect(&f,256,2,ELF_IMAGE_TRUNCATED);
    f=fixture(); put(f.bytes+96,33,8); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+72,UINT64_MAX,8); expect(&f,256,2,ELF_IMAGE_TRUNCATED);
    f=fixture(); put(f.bytes+80,UINT32_MAX-15,8); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+24,UINT64_C(0x100000000),8); expect(&f,256,2,ELF_IMAGE_UNSUPPORTED);
    f=fixture(); put(f.bytes+112,3,8); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); expect(&f,256,0,ELF_IMAGE_CAPACITY);
    f=fixture(); f.fault=1; expect(&f,256,2,ELF_IMAGE_READ_FAILURE);
    f=fixture(); f.fault=2; expect(&f,256,2,ELF_IMAGE_TRUNCATED);
    f=fixture(); f.fault=3; expect(&f,256,2,ELF_IMAGE_READ_FAILURE);
    f=fixture(); put(f.bytes+120,1,4); expect(&f,256,1,ELF_IMAGE_CAPACITY);
    f=fixture(); put(f.bytes+56,0xffff,2); expect(&f,256,2,ELF_IMAGE_UNSUPPORTED);
    f=fixture(); put(f.bytes+52,63,2); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+160,7,8); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+64,7,4); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+136,UINT64_C(0x100000000),8); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+152,UINT64_MAX,8); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+112,32,8); put(f.bytes+80,0x10010,8);
    expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+56,1025,2); expect(&f,256,2,ELF_IMAGE_UNSUPPORTED);
    f=fixture(); put(f.bytes+64,0,4); expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); f.bytes[6]=0; expect(&f,256,2,ELF_IMAGE_MALFORMED);
    f=fixture(); put(f.bytes+16,3,2); expect(&f,256,2,ELF_IMAGE_UNSUPPORTED);
    /* Unknown program headers need no segment interpretation; no TLS is valid. */
    f=fixture(); put(f.bytes+120,0x60000000,4); put(f.bytes+136,UINT64_MAX,8);
    { ElfImageSegment s[1]; ElfImagePlan p; ElfImageReader r={read_fixture,&f,256};
      assert(elf_image_plan(&r,s,1,&p)==ELF_IMAGE_OK);
      assert(!p.has_tls && p.segment_count==1); ++tests; }
    /* US EBOOT has zero-sized LOAD placeholders whose offset and vaddr
     * differ modulo alignment. No bytes are mapped, so congruence is moot. */
    f=fixture(); put(f.bytes+120,1,4); put(f.bytes+128,200,8);
    put(f.bytes+136,0,8); put(f.bytes+152,0,8); put(f.bytes+160,0,8);
    put(f.bytes+168,0x10000,8);
    { ElfImageSegment s[1]; ElfImagePlan p; ElfImageReader r={read_fixture,&f,256};
      assert(elf_image_plan(&r,s,1,&p)==ELF_IMAGE_OK);
      assert(!p.has_tls && p.segment_count==1); ++tests; }
    /* A segment ending exactly at the 4 GiB boundary is representable. */
    f=fixture(); put(f.bytes+80,UINT32_MAX-31,8); put(f.bytes+136,UINT32_MAX-23,8);
    { ElfImageSegment s[2]; ElfImagePlan p; ElfImageReader r={read_fixture,&f,256};
      assert(elf_image_plan(&r,s,2,&p)==ELF_IMAGE_OK); ++tests; }
    assert(elf_image_plan(NULL,NULL,0,NULL)==ELF_IMAGE_ARGUMENT); ++tests;
    test_process_parameters();
    printf("ELF_IMAGE_PASS tests=%d\n",tests); return 0;
}

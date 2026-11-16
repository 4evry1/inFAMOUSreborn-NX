/* Decision 33 / B2b: the SPU image and SPU thread group domain (native/domain/lv2_spu.h), on its own.
 *
 * Real guard, caller-provided local-store pool, no CPU and no guest memory: the domain is
 * guest-memory-free by construction, and the ABI (big-endian writes, r3 sign extension, EFAULT writing
 * nothing) belongs to the adapter tests. Every expectation is derived from the documented lv2
 * semantics, never captured from a run. */
#include "../domain/lv2_spu.h"
extern "C" {
#include "../adapters/guard_pthread.h"
}
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures;
#define CHECK(value) do { if (!(value)) { std::printf("LV2_SPU_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)

constexpr uint32_t USABLE_SPUS = 6;

struct Runtime {
    ps3_pthread_guard guard, image_guard, group_guard;
    ps3_lv2_spu_images images{};
    ps3_lv2_spu_groups groups{};
    std::vector<unsigned char> pool = std::vector<unsigned char>(
        size_t(PS3_LV2_SPU_GROUP_CAP) * PS3_LV2_SPU_THREADS_MAX * PS3_LV2_SPU_LS_BYTES);
    Runtime()
    {
        CHECK(ps3_pthread_guard_init(&guard) == 0);
        CHECK(ps3_pthread_guard_init(&image_guard) == 0);
        CHECK(ps3_pthread_guard_init(&group_guard) == 0);
        CHECK(ps3_lv2_spu_images_init(&images, ps3_pthread_guard_port(&image_guard)) == PS3_LV2_SPU_OK);
        CHECK(ps3_lv2_spu_groups_init(&groups, ps3_pthread_guard_port(&group_guard), pool.data(), USABLE_SPUS) == PS3_LV2_SPU_OK);
    }
    ~Runtime()
    {
        ps3_pthread_guard_destroy(&guard);
        ps3_pthread_guard_destroy(&image_guard);
        ps3_pthread_guard_destroy(&group_guard);
    }
};

/* ---------- a hand-built SPU ELF ----------
 * ELF32 big-endian, e_machine 0x17. Two PT_LOAD: the first has filesz == memsz (COPY only), the
 * second has filesz < memsz so the parser must also emit a zero FILL for its bss tail. */
struct CraftedElf {
    std::vector<unsigned char> bytes;
    std::vector<unsigned char> payload_a, payload_b;
    uint32_t entry = 0x1234, va_a = 0x100, va_b = 0x1000;
};

void put16(std::vector<unsigned char> &bytes, size_t at, uint32_t value)
{
    bytes[at] = uint8_t(value >> 8); bytes[at + 1] = uint8_t(value);
}
void put32(std::vector<unsigned char> &bytes, size_t at, uint32_t value)
{
    bytes[at] = uint8_t(value >> 24); bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8); bytes[at + 3] = uint8_t(value);
}

CraftedElf craft_elf()
{
    CraftedElf elf;
    elf.payload_a = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    elf.payload_b = {0xAA, 0xBB, 0xCC, 0xDD};
    const size_t header = 0x34, phoff = header, phentsize = 0x20, phnum = 2;
    const size_t a_off = phoff + phentsize * phnum;
    const size_t b_off = a_off + elf.payload_a.size();
    elf.bytes.assign(b_off + elf.payload_b.size(), 0);
    elf.bytes[0] = 0x7F; elf.bytes[1] = 'E'; elf.bytes[2] = 'L'; elf.bytes[3] = 'F';
    elf.bytes[4] = 1;                                  // ELFCLASS32
    elf.bytes[5] = 2;                                  // ELFDATA2MSB (big-endian)
    put16(elf.bytes, 0x12, 0x17);                      // e_machine: EM_SPU
    put32(elf.bytes, 0x18, elf.entry);                 // e_entry
    put32(elf.bytes, 0x1C, uint32_t(phoff));           // e_phoff
    put16(elf.bytes, 0x2A, uint32_t(phentsize));       // e_phentsize
    put16(elf.bytes, 0x2C, uint32_t(phnum));           // e_phnum
    // PT_LOAD 0: COPY, filesz == memsz.
    put32(elf.bytes, phoff + 0x00, 1);
    put32(elf.bytes, phoff + 0x04, uint32_t(a_off));
    put32(elf.bytes, phoff + 0x08, elf.va_a);
    put32(elf.bytes, phoff + 0x10, uint32_t(elf.payload_a.size()));
    put32(elf.bytes, phoff + 0x14, uint32_t(elf.payload_a.size()));
    // PT_LOAD 1: COPY + a 0x40-byte zero bss tail.
    const size_t ph1 = phoff + phentsize;
    put32(elf.bytes, ph1 + 0x00, 1);
    put32(elf.bytes, ph1 + 0x04, uint32_t(b_off));
    put32(elf.bytes, ph1 + 0x08, elf.va_b);
    put32(elf.bytes, ph1 + 0x10, uint32_t(elf.payload_b.size()));
    put32(elf.bytes, ph1 + 0x14, uint32_t(elf.payload_b.size()) + 0x40);
    std::memcpy(elf.bytes.data() + a_off, elf.payload_a.data(), elf.payload_a.size());
    std::memcpy(elf.bytes.data() + b_off, elf.payload_b.data(), elf.payload_b.size());
    return elf;
}

uint32_t seg32(const std::vector<unsigned char> &segments, size_t index, size_t offset)
{
    const size_t at = index * PS3_LV2_SPU_SEGMENT_BYTES + offset;
    return uint32_t(segments[at]) << 24 | uint32_t(segments[at + 1]) << 16 |
        uint32_t(segments[at + 2]) << 8 | uint32_t(segments[at + 3]);
}

/* ---------- images ---------- */

void image_import_builds_big_endian_segments()
{
    Runtime r;
    const CraftedElf elf = craft_elf();
    std::vector<unsigned char> segments(size_t(PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) * PS3_LV2_SPU_SEGMENT_BYTES, 0xEE);
    ps3_lv2_spu_image image{};
    uint32_t written = 0;
    CHECK(ps3_lv2_spu_image_parse(&r.images, elf.bytes.data(), elf.bytes.size(), &image,
        segments.data(), segments.size(), &written) == PS3_LV2_SPU_OK);
    CHECK(written == 3);                       // COPY, COPY, FILL
    CHECK(image.entry_point == elf.entry);
    CHECK(image.nsegs == 3);
    /* Segment 0: COPY of payload_a at va_a. */
    CHECK(seg32(segments, 0, 0x00) == 1);
    CHECK(seg32(segments, 0, 0x04) == elf.va_a);
    CHECK(seg32(segments, 0, 0x08) == elf.payload_a.size());
    /* Segment 1: COPY of payload_b at va_b. */
    CHECK(seg32(segments, 1, 0x00) == 1);
    CHECK(seg32(segments, 1, 0x04) == elf.va_b);
    CHECK(seg32(segments, 1, 0x08) == elf.payload_b.size());
    /* Segment 2: the FILL bss tail, zero sized source. */
    CHECK(seg32(segments, 2, 0x00) == 2);
    CHECK(seg32(segments, 2, 0x04) == elf.va_b + elf.payload_b.size());
    CHECK(seg32(segments, 2, 0x08) == 0x40);
    CHECK(seg32(segments, 2, 0x10) == 0);
    /* The source word is big-endian in BOTH halves so a 32- or 64-bit reader agrees. */
    CHECK(seg32(segments, 0, 0x10) == seg32(segments, 0, 0x14));
}

void image_import_rejects_a_bad_elf()
{
    Runtime r;
    std::vector<unsigned char> segments(size_t(PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) * PS3_LV2_SPU_SEGMENT_BYTES);
    ps3_lv2_spu_image image{};
    uint32_t written = 0;
    CraftedElf elf = craft_elf();
    std::vector<unsigned char> bad_magic = elf.bytes;
    bad_magic[0] = 'X';
    CHECK(ps3_lv2_spu_image_parse(&r.images, bad_magic.data(), bad_magic.size(), &image,
        segments.data(), segments.size(), &written) == PS3_LV2_SPU_EINVAL);
    std::vector<unsigned char> bad_machine = elf.bytes;
    put16(bad_machine, 0x12, 0x15);   // not SPU
    CHECK(ps3_lv2_spu_image_parse(&r.images, bad_machine.data(), bad_machine.size(), &image,
        segments.data(), segments.size(), &written) == PS3_LV2_SPU_EINVAL);
    /* Truncated: shorter than the ELF header. */
    CHECK(ps3_lv2_spu_image_parse(&r.images, elf.bytes.data(), 0x10, &image,
        segments.data(), segments.size(), &written) == PS3_LV2_SPU_EINVAL);
    /* A null buffer is EINVAL, not a crash. */
    CHECK(ps3_lv2_spu_image_parse(&r.images, nullptr, 0, &image, segments.data(), segments.size(), &written) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_image_parse(&r.images, elf.bytes.data(), elf.bytes.size(), nullptr,
        segments.data(), segments.size(), &written) == PS3_LV2_SPU_EFAULT);
}

void image_register_and_close()
{
    Runtime r;
    const CraftedElf elf = craft_elf();
    std::vector<unsigned char> segments(size_t(PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) * PS3_LV2_SPU_SEGMENT_BYTES);
    ps3_lv2_spu_image image{};
    uint32_t written = 0;
    CHECK(ps3_lv2_spu_image_parse(&r.images, elf.bytes.data(), elf.bytes.size(), &image,
        segments.data(), segments.size(), &written) == PS3_LV2_SPU_OK);
    image.segs_ea = 0x0D000000;      // where the adapter placed the guest copy
    image.source_ea = 0x30020380;
    CHECK(ps3_lv2_spu_image_register(&r.images, &image) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_image_register(&r.images, &image) == PS3_LV2_SPU_OK);   // idempotent
    ps3_lv2_spu_image found{};
    CHECK(ps3_lv2_spu_image_find(&r.images, 0x0D000000, &found) == PS3_LV2_SPU_OK);
    CHECK(found.entry_point == image.entry_point && found.nsegs == image.nsegs);
    CHECK(ps3_lv2_spu_image_find(&r.images, 0x0D100000, &found) == PS3_LV2_SPU_ESRCH);
    CHECK(ps3_lv2_spu_image_close(&r.images, 0x0D000000) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_image_close(&r.images, 0x0D000000) == PS3_LV2_SPU_ESRCH);   // already closed
    CHECK(ps3_lv2_spu_image_find(&r.images, 0x0D000000, &found) == PS3_LV2_SPU_ESRCH);
    CHECK(ps3_lv2_spu_image_find(&r.images, 0, nullptr) == PS3_LV2_SPU_EFAULT);
}

/* ---------- groups ---------- */

uint32_t make_group(Runtime &r, uint32_t num)
{
    uint32_t id = 0;
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, num, 100, 0) == PS3_LV2_SPU_OK);
    CHECK(id != 0);
    return id;
}

void group_create_limits()
{
    Runtime r;
    uint32_t id = 0;
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, 0, 100, 0) == PS3_LV2_SPU_EINVAL);       // no threads
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, PS3_LV2_SPU_THREADS_MAX + 1, 100, 0) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, 2, 15, 0) == PS3_LV2_SPU_EINVAL);        // prio too low
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, 2, 256, 0) == PS3_LV2_SPU_EINVAL);       // prio too high
    CHECK(ps3_lv2_spu_group_create(&r.groups, nullptr, 2, 100, 0) == PS3_LV2_SPU_EFAULT);
    /* The policy's usable-SPU count bounds creation (6 here). */
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, USABLE_SPUS + 1, 100, 0) == PS3_LV2_SPU_EAGAIN);
    CHECK(ps3_lv2_spu_group_create(&r.groups, &id, USABLE_SPUS, 100, 0) == PS3_LV2_SPU_OK);
    /* Group-table exhaustion is EAGAIN, never a made-up id. */
    uint32_t extra = 0;
    size_t created = 1;
    while (ps3_lv2_spu_group_create(&r.groups, &extra, 1, 100, 0) == PS3_LV2_SPU_OK) ++created;
    CHECK(created == size_t(PS3_LV2_SPU_GROUP_CAP));
    CHECK(ps3_lv2_spu_group_create(&r.groups, &extra, 1, 100, 0) == PS3_LV2_SPU_EAGAIN);
    /* A lower usable count refuses immediately. */
    ps3_lv2_spu_groups_set_usable(&r.groups, 1);
    CHECK(ps3_lv2_spu_group_create(&r.groups, &extra, 2, 100, 0) == PS3_LV2_SPU_EAGAIN);
}

struct Fixture {
    CraftedElf elf;
    std::vector<unsigned char> segments;
    ps3_lv2_spu_image image{};
};

Fixture import_into(Runtime &r)
{
    Fixture fixture;
    fixture.elf = craft_elf();
    fixture.segments.assign(size_t(PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) * PS3_LV2_SPU_SEGMENT_BYTES, 0);
    uint32_t written = 0;
    CHECK(ps3_lv2_spu_image_parse(&r.images, fixture.elf.bytes.data(), fixture.elf.bytes.size(), &fixture.image,
        fixture.segments.data(), fixture.segments.size(), &written) == PS3_LV2_SPU_OK);
    return fixture;
}

void thread_initialize_copies_into_the_local_store()
{
    Runtime r;
    const Fixture fixture = import_into(r);
    const uint32_t group = make_group(r, 2);
    const uint64_t args[4] = {0x1111, 0x2222, 0x3333, 0x4444};
    uint32_t thread = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &thread, group, 0, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    CHECK(thread != 0);
    /* Byte for byte: payload A at va_a, payload B at va_b, and the bss tail zero. */
    uint64_t value = 0;
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, fixture.elf.va_a, &value, 4) == PS3_LV2_SPU_OK);
    CHECK(value == 0xDEADBEEF);
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, fixture.elf.va_b, &value, 4) == PS3_LV2_SPU_OK);
    CHECK(value == 0xAABBCCDD);
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, fixture.elf.va_b + 8, &value, 8) == PS3_LV2_SPU_OK);
    CHECK(value == 0);   // past the payload: the FILL tail, zeroed
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, 0, &value, 2) == PS3_LV2_SPU_OK);
    CHECK(value == 0);   // untouched LS below the first segment
    /* A second thread's store is independent and starts zeroed. */
    uint32_t other = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &other, group, 1, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    CHECK(other != thread);
    CHECK(ps3_lv2_spu_thread_write_ls(&r.groups, thread, 0x2000, 0x11223344, 4) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, other, 0x2000, &value, 4) == PS3_LV2_SPU_OK);
    CHECK(value == 0);
    /* The spu number must be inside the group. */
    uint32_t spare = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &spare, group, 2, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_EINVAL);
}

void read_and_write_ls_sizes_and_alignment()
{
    Runtime r;
    const Fixture fixture = import_into(r);
    const uint32_t group = make_group(r, 1);
    const uint64_t args[4] = {0, 0, 0, 0};
    uint32_t thread = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &thread, group, 0, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    uint64_t value = 0;
    for (size_t size : {size_t(1), size_t(2), size_t(4), size_t(8)}) {
        const uint64_t pattern = size == 8 ? UINT64_C(0x0102030405060708) : (UINT64_C(0x01) << (8 * size)) - 1;
        CHECK(ps3_lv2_spu_thread_write_ls(&r.groups, thread, 0x100, pattern, size) == PS3_LV2_SPU_OK);
        CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, 0x100, &value, size) == PS3_LV2_SPU_OK);
        CHECK(value == pattern);
    }
    /* Alignment and size rules. */
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, 0x101, &value, 4) == PS3_LV2_SPU_EINVAL);   // misaligned
    CHECK(ps3_lv2_spu_thread_write_ls(&r.groups, thread, 0x101, 0, 2) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, 0x100, &value, 3) == PS3_LV2_SPU_EINVAL);   // bad size
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, PS3_LV2_SPU_LS_BYTES - 2, &value, 4) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, thread, 0, nullptr, 4) == PS3_LV2_SPU_EFAULT);
    CHECK(ps3_lv2_spu_thread_read_ls(&r.groups, 0xDEADBEEF, 0, &value, 4) == PS3_LV2_SPU_ESRCH);
}

void connect_event_all_threads_picks_a_port()
{
    Runtime r;
    const Fixture fixture = import_into(r);
    const uint32_t group = make_group(r, 2);
    const uint64_t args[4] = {0, 0, 0, 0};
    uint32_t first = 0, second = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &first, group, 0, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &second, group, 1, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    uint32_t spu_number = 0xFFFF;
    /* Root fix: lv2 semantics -- the lowest REQUESTED spup not yet bound by any thread of the group. */
    CHECK(ps3_lv2_spu_group_connect_event_all_threads(&r.groups, group, 0x0E000000, 0x1, &spu_number) == PS3_LV2_SPU_OK);
    CHECK(spu_number == 0);
    CHECK(ps3_lv2_spu_group_connect_event_all_threads(&r.groups, group, 0x0E000100, 0x1, &spu_number) == PS3_LV2_SPU_EISCONN);
    CHECK(ps3_lv2_spu_group_connect_event_all_threads(&r.groups, group, 0x0E000100, 0x6, &spu_number) == PS3_LV2_SPU_OK);
    CHECK(spu_number == 1);
    CHECK(ps3_lv2_spu_group_connect_event_all_threads(&r.groups, group, 0x0E000000, 0, &spu_number) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_group_connect_event_all_threads(&r.groups, group, 0x0E000000, 0x1, nullptr) == PS3_LV2_SPU_EFAULT);
    CHECK(ps3_lv2_spu_group_connect_event_all_threads(&r.groups, 0xDEADBEEF, 0, 0x1, &spu_number) == PS3_LV2_SPU_ESRCH);
}

void destroy_before_start_is_allowed()
{
    Runtime r;
    const uint32_t group = make_group(r, 2);
    CHECK(ps3_lv2_spu_group_destroy(&r.groups, group) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_group_destroy(&r.groups, group) == PS3_LV2_SPU_ESRCH);
    /* Event and priority helpers on a dead group. */
    CHECK(ps3_lv2_spu_group_set_priority(&r.groups, group, 100) == PS3_LV2_SPU_ESRCH);
    uint32_t priority = 0;
    CHECK(ps3_lv2_spu_group_get_priority(&r.groups, group, &priority) == PS3_LV2_SPU_ESRCH);
}

void start_with_an_uninitialized_thread_is_estat()
{
    Runtime r;
    const Fixture fixture = import_into(r);
    const uint32_t group = make_group(r, 2);
    const uint64_t args[4] = {0, 0, 0, 0};
    uint32_t thread = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &thread, group, 0, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    /* Thread 1 was never initialized. */
    CHECK(ps3_lv2_spu_group_start(&r.groups, group) == PS3_LV2_SPU_ESTAT);
    CHECK(ps3_lv2_spu_group_start(&r.groups, 0xDEADBEEF) == PS3_LV2_SPU_ESRCH);
}

void start_records_the_snapshot()
{
    Runtime r;
    const Fixture fixture = import_into(r);
    const uint32_t group = make_group(r, 2);
    uint64_t args[4] = {0xAAAA0001, 0xBBBB0002, 0xCCCC0003, 0xDDDD0004};
    uint32_t first = 0, second = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &first, group, 0, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    args[0] = 0xAAAA0005;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &second, group, 1, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    ps3_lv2_spu_start_snapshot before{};
    CHECK(ps3_lv2_spu_group_snapshot(&r.groups, &before) == PS3_LV2_SPU_OK);
    CHECK(before.valid == 0);   // nothing started yet
    CHECK(ps3_lv2_spu_group_start(&r.groups, group) == PS3_LV2_SPU_OK);
    ps3_lv2_spu_start_snapshot snapshot{};
    CHECK(ps3_lv2_spu_group_snapshot(&r.groups, &snapshot) == PS3_LV2_SPU_OK);
    CHECK(snapshot.valid == 1);
    CHECK(snapshot.group == group && snapshot.num == 2 && snapshot.priority == 100);
    CHECK(snapshot.thread_id[0] == first && snapshot.thread_id[1] == second);
    CHECK(snapshot.entry[0] == fixture.image.entry_point && snapshot.entry[1] == fixture.image.entry_point);
    CHECK(snapshot.arg1[0] == UINT64_C(0xAAAA0001));
    CHECK(snapshot.arg1[1] == UINT64_C(0xAAAA0005));
    /* The hash covers the first 64 bytes of that thread's own local store, and the two differ only
     * if their stores do -- identical stores hash the same. */
    CHECK(snapshot.ls_hash[0] == snapshot.ls_hash[1]);
    CHECK(ps3_lv2_spu_thread_write_ls(&r.groups, second, 0x2000, 0x1, 1) == PS3_LV2_SPU_OK);
    ps3_lv2_spu_group_start(&r.groups, group);   // second start must refuse
    /* A started group refuses a second start and a destroy. */
    CHECK(ps3_lv2_spu_group_start(&r.groups, group) == PS3_LV2_SPU_EBUSY);
    CHECK(ps3_lv2_spu_group_destroy(&r.groups, group) == PS3_LV2_SPU_EBUSY);
    CHECK(ps3_lv2_spu_group_create(&r.groups, nullptr, 1, 100, 0) == PS3_LV2_SPU_EFAULT);
}

void priority_and_event_helpers_round_trip()
{
    Runtime r;
    const uint32_t group = make_group(r, 1);
    CHECK(ps3_lv2_spu_group_set_priority(&r.groups, group, 200) == PS3_LV2_SPU_OK);
    uint32_t priority = 0;
    CHECK(ps3_lv2_spu_group_get_priority(&r.groups, group, &priority) == PS3_LV2_SPU_OK && priority == 200);
    CHECK(ps3_lv2_spu_group_set_priority(&r.groups, group, 15) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_group_get_priority(&r.groups, group, nullptr) == PS3_LV2_SPU_EFAULT);
    /* Group-level connects take one port per documented event type. */
    CHECK(ps3_lv2_spu_group_connect_event(&r.groups, group, 0x0E000100, 1) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_group_connect_event(&r.groups, group, 0x0E000200, 2) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_group_connect_event(&r.groups, group, 0x0E000300, 8) == PS3_LV2_SPU_EINVAL);
    CHECK(ps3_lv2_spu_group_disconnect_event(&r.groups, group) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_group_disconnect_event(&r.groups, group) == PS3_LV2_SPU_EINVAL);   // nothing connected
    /* Terminate and join before start are a state error, never a hang. */
    CHECK(ps3_lv2_spu_group_terminate(&r.groups, group) == PS3_LV2_SPU_ESTAT);
    CHECK(ps3_lv2_spu_group_join(&r.groups, group) == PS3_LV2_SPU_ESTAT);
}

void exit_status_and_set_argument()
{
    Runtime r;
    const Fixture fixture = import_into(r);
    const uint32_t group = make_group(r, 1);
    const uint64_t args[4] = {0, 0, 0, 0};
    uint32_t thread = 0;
    CHECK(ps3_lv2_spu_thread_initialize(&r.groups, &thread, group, 0, &fixture.image, fixture.segments.data(),
        fixture.image.nsegs, fixture.elf.bytes.data(), fixture.elf.bytes.size(), args) == PS3_LV2_SPU_OK);
    uint32_t status = 0xFFFF;
    /* Nothing has run, so there is no exit status: ESTAT, never a fabricated 0. */
    CHECK(ps3_lv2_spu_thread_get_exit_status(&r.groups, thread, &status) == PS3_LV2_SPU_ESTAT);
    const uint64_t new_args[4] = {9, 8, 7, 6};
    CHECK(ps3_lv2_spu_thread_set_argument(&r.groups, thread, new_args) == PS3_LV2_SPU_OK);
    CHECK(ps3_lv2_spu_thread_set_argument(&r.groups, 0xDEADBEEF, new_args) == PS3_LV2_SPU_ESRCH);
    CHECK(ps3_lv2_spu_thread_set_argument(&r.groups, thread, nullptr) == PS3_LV2_SPU_EFAULT);
    CHECK(ps3_lv2_spu_thread_get_exit_status(&r.groups, 0xDEADBEEF, &status) == PS3_LV2_SPU_ESRCH);
    CHECK(ps3_lv2_spu_thread_get_exit_status(&r.groups, thread, nullptr) == PS3_LV2_SPU_EFAULT);
}

}  // namespace

int main()
{
    image_import_builds_big_endian_segments();
    image_import_rejects_a_bad_elf();
    image_register_and_close();
    group_create_limits();
    thread_initialize_copies_into_the_local_store();
    read_and_write_ls_sizes_and_alignment();
    connect_event_all_threads_picks_a_port();
    destroy_before_start_is_allowed();
    start_with_an_uninitialized_thread_is_estat();
    start_records_the_snapshot();
    priority_and_event_helpers_round_trip();
    exit_status_and_set_argument();
    std::printf("LV2_SPU_RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

#include "../adapters/entry_checkpoint.h"
#include "guest_space.h"
#include "elf_image.h"
#include "tls_initialize.h"
#include <switch.h>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t STACK_TOP = 0xd0100000;
constexpr uint32_t STACK_START = STACK_TOP - 0x10000;
constexpr size_t STACK_BYTES = 0x11000;
constexpr uint32_t ARGV_ADDRESS = 0x00b00000;
constexpr uint32_t TLS_START = 0x0e000000;
constexpr size_t TLS_BYTES = 0x9000;
constexpr uint32_t TLS_NID = 0x744680a2;
constexpr const char BOOT_PATH[] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
struct Snapshot { uint32_t address; size_t length; unsigned char *expected; };
ElfReadStatus read_file(void *context, uint64_t offset, void *destination, size_t length)
{
    FILE *file = static_cast<FILE *>(context);
    if (offset > LONG_MAX || std::fseek(file, static_cast<long>(offset), SEEK_SET)) return ELF_READ_ERROR;
    return std::fread(destination, 1, length, file) == length ? ELF_READ_OK : ELF_READ_ERROR;
}
void encode(unsigned char *destination, uint64_t value, unsigned bytes)
{
    for (unsigned index = 0; index < bytes; ++index) {
        destination[index] = static_cast<unsigned char>(value >> ((bytes - 1 - index) * 8));
    }
}
uint32_t decode32(const unsigned char *source)
{
    return (uint32_t(source[0]) << 24) | (uint32_t(source[1]) << 16) |
           (uint32_t(source[2]) << 8) | uint32_t(source[3]);
}
bool read_word(const ps3_guest_memory *memory, uint32_t address, uint32_t *value)
{
    unsigned char bytes[4];
    if (memory->read(memory->context, address, bytes, 4) != PS3_GUEST_MEMORY_OK) return false;
    *value = decode32(bytes);
    return true;
}
bool compare_snapshot(const ps3_guest_memory *memory, const Snapshot &snapshot)
{
    unsigned char actual[4096];
    for (size_t offset = 0; offset < snapshot.length;) {
        size_t count = snapshot.length - offset;
        if (count > sizeof(actual)) count = sizeof(actual);
        if (memory->read(memory->context, snapshot.address + offset, actual, count) != PS3_GUEST_MEMORY_OK ||
            std::memcmp(actual, snapshot.expected + offset, count)) return false;
        offset += count;
    }
    return true;
}
bool patch_image(Snapshot *images, size_t count, uint32_t address, uint32_t value)
{
    for (size_t index = 0; index < count; ++index) {
        if (address >= images[index].address && address - images[index].address <= images[index].length &&
            4 <= images[index].length - (address - images[index].address)) {
            encode(images[index].expected + address - images[index].address, value, 4);
            return true;
        }
    }
    return false;
}
bool grant_page_global_span(EntryCapture *capture, const ps3_guest_memory *memory,
                            const Snapshot *images, size_t count, uint32_t page_global)
{
    if (page_global > UINT32_MAX - 3) return false;
    bool covered = false;
    for (size_t index = 0; index < count; ++index) {
        if (page_global >= images[index].address &&
            page_global - images[index].address <= images[index].length &&
            4 <= images[index].length - (page_global - images[index].address)) {
            covered = true;
            break;
        }
    }
    if (!covered) return false;
    unsigned char current[4];
    if (memory->read(memory->context, page_global, current, sizeof(current)) != PS3_GUEST_MEMORY_OK) {
        return false;
    }
    capture->spans[3] = {page_global, sizeof(current), true};
    return true;
}

bool run_worker(EntryCapture *capture, void (*entry)(void *) = native_entry_checkpoint_worker)
{
    Thread worker;
    Result result = threadCreate(&worker, entry, capture,
                                 nullptr, 0x20000, 0x2c, -2);
    if (R_FAILED(result)) return false;
    result = threadStart(&worker);
    if (R_FAILED(result)) { threadClose(&worker); return false; }
    result = threadWaitForExit(&worker);
    if (R_FAILED(result)) {
        std::printf("ENTRY_WORKER_JOIN_FAILED result=0x%x\n", result);
        std::abort();
    }
    const Result closed = threadClose(&worker);
    if (R_FAILED(closed)) {
        std::printf("ENTRY_WORKER_CLOSE_FAILED result=0x%x\n", closed);
        std::abort();
    }
    return true;
}
}

extern "C" int native_test_entry_checkpoint(const ps3_guest_memory *memory, uintptr_t base, const char *path)
{
    int failures = 0;
#define REQUIRE(condition) do { if (!(condition)) { std::printf("ENTRY_CHECKPOINT_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    FILE *file = std::fopen(path, "rb");
    if (!file) return 1;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return 1;
    }
    const long file_size = std::ftell(file);
    if (file_size <= 0) {
        std::fclose(file);
        return 1;
    }
    ElfImageReader reader = {read_file, file, uint64_t(file_size)};
    ElfImageSegment segments[ELF_IMAGE_MAX_SEGMENTS];
    ElfImagePlan plan = {};
    const ElfImageStatus planned = elf_image_plan(&reader, segments, ELF_IMAGE_MAX_SEGMENTS, &plan);
    std::fclose(file);
    if (planned != ELF_IMAGE_OK || plan.segment_count != 2 || !plan.has_tls ||
        plan.entry_opd != 0x771b08 || plan.tls.guest_address != 0x785e7c ||
        plan.tls.file_size != 4 || plan.tls.memory_size != 0x200) return 1;
    Snapshot images[2] = {};
    size_t total_image_bytes = 0;
    for (size_t index = 0; index < 2; ++index) {
        if (segments[index].memory_size > 16 * 1024 * 1024 ||
            uint64_t(segments[index].guest_address) + segments[index].memory_size >= ARGV_ADDRESS) return 1;
        images[index] = {segments[index].guest_address, size_t(segments[index].memory_size), nullptr};
        total_image_bytes += images[index].length;
    }
    for (size_t index = 0; index < 2; ++index) {
        images[index].expected = static_cast<unsigned char *>(std::malloc(images[index].length));
        if (!images[index].expected || memory->read(memory->context, images[index].address,
            images[index].expected, images[index].length) != PS3_GUEST_MEMORY_OK) {
            for (Snapshot &image : images) std::free(image.expected);
            return 1;
        }
    }
    unsigned char *stack = static_cast<unsigned char *>(std::malloc(STACK_BYTES));
    if (!stack) { for (Snapshot &image : images) std::free(image.expected); return 1; }
    std::memset(stack, 0xa5, STACK_BYTES);
    unsigned char argv[4096] = {};
    encode(argv, ARGV_ADDRESS + 0x20, 8);
    std::memcpy(argv + 0x20, BOOT_PATH, sizeof(BOOT_PATH));
    REQUIRE(memory->commit(memory->context, STACK_START, STACK_BYTES) == PS3_GUEST_MEMORY_OK);
    REQUIRE(memory->commit(memory->context, ARGV_ADDRESS, sizeof(argv)) == PS3_GUEST_MEMORY_OK);
    REQUIRE(memory->write(memory->context, STACK_START, stack, STACK_BYTES) == PS3_GUEST_MEMORY_OK);
    REQUIRE(memory->write(memory->context, ARGV_ADDRESS, argv, sizeof(argv)) == PS3_GUEST_MEMORY_OK);
    uint32_t code = 0, toc = 0, page_global = 0;
    REQUIRE(read_word(memory, plan.entry_opd, &code));
    REQUIRE(read_word(memory, plan.entry_opd + 4, &toc));
    REQUIRE(code == 0x10230 && toc == 0x78bf78);
    REQUIRE(read_word(memory, toc - 0x7ffc, &page_global));
    REQUIRE(page_global == 0x0078ee2c);
    unsigned char tls_source[4] = {};
    REQUIRE(memory->read(memory->context, plan.tls.guest_address, tls_source, 4) == PS3_GUEST_MEMORY_OK);
    if (failures) {
        std::free(stack);
        for (Snapshot &image : images) std::free(image.expected);
        return 1;
    }
    EntryCapture capture = {};
    capture.base = base;
    capture.spans[0] = {plan.entry_opd + 4, 4, false};
    capture.spans[1] = {toc - 0x7ffc, 4, false};
    capture.spans[2] = {STACK_START, STACK_BYTES, true};
    // Model malformed ELF-derived data, then restore the exact original bytes.
    unsigned char malformed_pointer[4], original_pointer[4];
    encode(malformed_pointer, 0xc0000000, 4);
    encode(original_pointer, page_global, 4);
    REQUIRE(memory->write(memory->context, toc - 0x7ffc, malformed_pointer, 4) == PS3_GUEST_MEMORY_OK);
    uint32_t malformed_global = 0;
    REQUIRE(read_word(memory, toc - 0x7ffc, &malformed_global));
    unsigned char unmapped_probe[4];
    REQUIRE(memory->read(memory->context, malformed_global, unmapped_probe, 4) == PS3_GUEST_MEMORY_UNMAPPED);
    REQUIRE(!grant_page_global_span(&capture, memory, images, 2, malformed_global));
    REQUIRE(capture.spans[3].length == 0);
    REQUIRE(memory->write(memory->context, toc - 0x7ffc, original_pointer, 4) == PS3_GUEST_MEMORY_OK);
    if (failures) {
        std::printf("ENTRY_PAGE_GLOBAL_PREFLIGHT_FAIL worker_started=false pointer_restored=true\n");
        std::free(stack);
        for (Snapshot &image : images) std::free(image.expected);
        return 1;
    }
    REQUIRE(grant_page_global_span(&capture, memory, images, 2, page_global));
    std::printf("ENTRY_PAGE_GLOBAL_PREFLIGHT_PASS malformed_pointer=0xc0000000 worker_started=false pointer_restored=true actual_pointer=0x%x\n", page_global);
    capture.context.gpr[1] = STACK_TOP;
    capture.context.gpr[2] = toc;
    capture.context.gpr[3] = 1;
    capture.context.gpr[4] = ARGV_ADDRESS;
    capture.context.gpr[7] = 1;
    capture.context.gpr[8] = plan.tls.guest_address;
    capture.context.gpr[9] = plan.tls.file_size;
    capture.context.gpr[10] = plan.tls.memory_size;
    capture.context.gpr[12] = 0x100000;
    capture.context.gpr[13] = 0x10f07000;
    capture.context.thread_id = 1;
    capture.context.cia = code;
    const ppu_context initial = capture.context;
    const size_t trace_capacity = sizeof(capture.accesses) / sizeof(capture.accesses[0]);
    capture.access_count = trace_capacity + 1;
    REQUIRE(run_worker(&capture));
    REQUIRE(capture.reason == ENTRY_TRACE_OVERFLOW && capture.access_count == trace_capacity + 1);
    ppu_context trace_expected = initial;
    trace_expected.gpr[2] = plan.entry_opd;
    REQUIRE(std::memcmp(&capture.context, &trace_expected, sizeof(initial)) == 0);
    capture.context = initial;
    capture.access_count = 0;
    std::printf("ENTRY_TRACE_BOUND_PASS corrupted_count=%zu joined=true\n", trace_capacity + 1);
    REQUIRE(run_worker(&capture, native_entry_unknown_import_worker));
    REQUIRE(capture.reason == ENTRY_UNSUPPORTED_IMPORT && capture.nid == 0xffffffff);
    REQUIRE(capture.access_count == 0 && std::memcmp(&capture.context, &initial, sizeof(initial)) == 0);
    REQUIRE(run_worker(&capture, native_entry_unreached_target_worker));
    REQUIRE(capture.reason == ENTRY_UNSUPPORTED_TARGET && capture.target == 0x00456420);
    REQUIRE(capture.access_count == 0 && std::memcmp(&capture.context, &initial, sizeof(initial)) == 0);
    std::printf("ENTRY_ADAPTER_REJECTION_PASS injected_unknown_nid=true injected_unreached_target=true joined=true\n");
    // A real unsupported stack access must stop before touching any backing bytes.
    capture.context.gpr[1] = 0x1000;
    REQUIRE(run_worker(&capture));
    REQUIRE(capture.reason == ENTRY_UNSUPPORTED_ACCESS && capture.invalid_address == 0xf90);
    ppu_context invalid_expected = initial;
    invalid_expected.gpr[1] = 0x1000;
    REQUIRE(std::memcmp(&capture.context, &invalid_expected, sizeof(initial)) == 0);
    for (const Snapshot &image : images) REQUIRE(compare_snapshot(memory, image));
    REQUIRE(compare_snapshot(memory, {STACK_START, STACK_BYTES, stack}));
    std::printf("ENTRY_UNSUPPORTED_ACCESS_PASS address=0x%llx joined=true\n", static_cast<unsigned long long>(capture.invalid_address));
    capture.context = initial;
    capture.reason = ENTRY_NOT_STOPPED;
    capture.access_count = 0;
    REQUIRE(run_worker(&capture));
    REQUIRE(capture.reason == ENTRY_IMPORT_DISPATCH && capture.nid == TLS_NID);
    ppu_context expected = initial;
    const uint32_t frame = STACK_TOP - 0x110;
    expected.gpr[0] = 0x10250;
    expected.gpr[1] = frame;
    expected.gpr[3] = 1;
    expected.gpr[4] = plan.tls.guest_address;
    expected.gpr[5] = plan.tls.file_size;
    expected.gpr[6] = plan.tls.memory_size;
    expected.gpr[11] = page_global;
    expected.gpr[26] = initial.gpr[5];
    expected.gpr[27] = ARGV_ADDRESS;
    expected.gpr[28] = ARGV_ADDRESS;
    expected.gpr[29] = 0x100000;
    expected.gpr[30] = initial.gpr[6];
    expected.gpr[31] = 1;
    expected.lr = 0x103a0;
    REQUIRE(std::memcmp(&capture.context, &expected, sizeof(expected)) == 0);
    EntryAccess accesses[] = {
        {plan.entry_opd + 4, toc, 4, false},
        {STACK_TOP - 0x70, STACK_TOP, 8, true}, {STACK_TOP - 0x70, 0, 8, true},
        {frame, STACK_TOP - 0x70, 8, true},
        {frame + 0x70, 0, 8, true}, {frame + 0x80, 0, 8, true},
        {frame + 0x90, 0, 8, true}, {frame + 0x98, 0, 8, true},
        {frame + 0x78, 0, 8, true}, {frame + 0x88, 0, 8, true},
        {frame + 0xb0, 0x10250, 8, true},
        {toc - 0x7ffc, page_global, 4, false}, {page_global, 0x100000, 4, true}
    };
    REQUIRE(capture.access_count == sizeof(accesses) / sizeof(accesses[0]));
    for (size_t index = 0; index < capture.access_count && index < sizeof(accesses) / sizeof(accesses[0]); ++index) {
        const EntryAccess &actual = capture.accesses[index];
        const EntryAccess &golden = accesses[index];
        REQUIRE(actual.address == golden.address && actual.value == golden.value &&
                actual.bytes == golden.bytes && actual.write == golden.write);
        std::printf("ENTRY_ACCESS index=%zu address=0x%llx bytes=%u write=%d value=0x%llx\n", index,
            static_cast<unsigned long long>(actual.address), actual.bytes, actual.write,
            static_cast<unsigned long long>(actual.value));
        if (golden.write && golden.address >= STACK_START && golden.address + golden.bytes <= STACK_START + STACK_BYTES) {
            encode(stack + golden.address - STACK_START, golden.value, golden.bytes);
        }
    }
    REQUIRE(patch_image(images, 2, page_global, 0x100000));
    std::printf("ENTRY_WORKER_STOP reason=%d nid=0x%x target=0x%x joined=true handler_returned=false\n", capture.reason, capture.nid, capture.target);
    if (failures == 0) {
        Ps3TlsArena arena = {TLS_START, TLS_START + 0x10000};
        Ps3TlsTemplate tls = {uint32_t(capture.context.gpr[4]), capture.context.gpr[5], capture.context.gpr[6]};
        Ps3TlsImage image = {};
        unsigned char scratch[4096];
        Ps3TlsReport report = ps3_tls_initialize(memory, &arena, &tls, scratch, sizeof(scratch), &image);
        REQUIRE(report.status == PS3_TLS_OK);
        REQUIRE(image.block == TLS_START && image.thread_pointer == TLS_START + 0x7000 &&
                image.allocation_size == TLS_BYTES && arena.next == TLS_START + TLS_BYTES);
        if (report.status == PS3_TLS_OK) {
            const uint64_t saved_toc = capture.context.gpr[2];
            unsigned char toc_bytes[8];
            encode(toc_bytes, saved_toc, 8);
            REQUIRE(memory->write(memory->context, frame + 0x28, toc_bytes, 8) == PS3_GUEST_MEMORY_OK);
            capture.context.gpr[13] = image.thread_pointer;
            capture.context.gpr[3] = 0;
            capture.context.gpr[2] = saved_toc;
            expected.gpr[13] = TLS_START + 0x7000;
            expected.gpr[3] = 0;
            encode(stack + frame + 0x28 - STACK_START, saved_toc, 8);
            REQUIRE(std::memcmp(&capture.context, &expected, sizeof(expected)) == 0);
            unsigned char actual[4096];
            for (size_t offset = 0; offset < TLS_BYTES; offset += sizeof(actual)) {
                REQUIRE(memory->read(memory->context, TLS_START + offset, actual, sizeof(actual)) == PS3_GUEST_MEMORY_OK);
                for (size_t byte = 0; byte < sizeof(actual); ++byte) {
                    const unsigned char wanted = offset + byte < 4 ? tls_source[offset + byte] : 0;
                    REQUIRE(actual[byte] == wanted);
                }
            }
            std::printf("ENTRY_TLS_EFFECTS status=%d block=0x%x tp=0x%x allocation=0x%llx root_completed=true\n",
                report.status, image.block, image.thread_pointer, static_cast<unsigned long long>(image.allocation_size));
        }
    }
    for (const Snapshot &image : images) REQUIRE(compare_snapshot(memory, image));
    REQUIRE(compare_snapshot(memory, {STACK_START, STACK_BYTES, stack}));
    REQUIRE(compare_snapshot(memory, {ARGV_ADDRESS, sizeof(argv), argv}));
    std::printf("ENTRY_CHECKPOINT_%s failures=%d whole_context_bytes=%zu image_bytes=%zu stack_bytes=%zu argv_bytes=%zu tls_bytes=%zu continuation_executed=false\n",
        failures ? "FAIL" : "PASS", failures, sizeof(ppu_context), total_image_bytes, STACK_BYTES, sizeof(argv), TLS_BYTES);
    std::free(stack);
    for (Snapshot &image : images) std::free(image.expected);
    return failures != 0;
}

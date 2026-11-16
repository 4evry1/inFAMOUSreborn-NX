#include "../adapters/ppu_memory_trace.h"
#include <cstdio>
#include <cstring>
#ifndef __SWITCH__
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

int main()
{
    int failures = 0;
#define CHECK(value) do { if (!(value)) { std::printf("TRACE_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    PpuMemoryTraceEvent wanted[3]{};
    wanted[0].origin = PpuMemoryTraceOrigin::Generated;
    wanted[0].operation = PpuMemoryTraceOperation::Read;
    wanted[0].address = 0x10000;
    wanted[0].length = 4;
    wanted[0].value = 7;
    wanted[1].origin = PpuMemoryTraceOrigin::Hle;
    wanted[1].operation = PpuMemoryTraceOperation::Write;
    wanted[1].address = 0xd0000028;
    wanted[1].length = 8;
    wanted[1].value = 0x78bf78;
    wanted[2].origin = PpuMemoryTraceOrigin::Generated;
    wanted[2].operation = PpuMemoryTraceOperation::Write;
    wanted[2].address = 0x10004;
    wanted[2].length = 4;
    wanted[2].value = 9;
    for (const auto &expected : wanted) {
        const auto actual = ppu_memory_scalar_event(expected.origin,
            expected.operation == PpuMemoryTraceOperation::Write,
            expected.address, expected.length, expected.value);
        CHECK(actual.origin == expected.origin);
        CHECK(actual.operation == expected.operation);
        CHECK(actual.address == expected.address && actual.length == expected.length);
        CHECK(actual.value == expected.value && actual.increment == 0);
        CHECK(!actual.stored && !actual.byte_transfer && actual.status == PS3_GUEST_MEMORY_OK);
        for (unsigned char byte : actual.bytes) CHECK(byte == 0);
    }
    PpuMemoryTraceEvent origin_only = wanted[1];
    origin_only.origin = PpuMemoryTraceOrigin::Generated;
    CHECK(!ppu_memory_event_equal(wanted[1], origin_only));
    CHECK(!ppu_memory_trace_equal(&wanted[1], 1, &origin_only, 1));
    PpuMemoryTraceEvent swapped[] = {wanted[0], wanted[2], wanted[1]};
    CHECK(ppu_memory_trace_equal(wanted, 3, wanted, 3));
    CHECK(ppu_memory_event_equal(wanted[0], swapped[0]));
    CHECK(ppu_memory_event_equal(wanted[2], swapped[1]));
    CHECK(ppu_memory_event_equal(wanted[1], swapped[2]));
    // Each filtered class retains its count/order; only cross-class order differs.
    CHECK(!ppu_memory_trace_equal(wanted, 3, swapped, 3));
    PpuMemoryTraceEvent storage[2]{};
    PpuMemoryTraceJournal journal{storage, 2, 0};
    CHECK(ppu_memory_trace_append(journal, wanted[0]));
    CHECK(ppu_memory_trace_append(journal, wanted[1]));
    CHECK(!ppu_memory_trace_append(journal, wanted[2]) && journal.count == 2);
    journal.count = 3;
    CHECK(!ppu_memory_trace_available(journal));
    CHECK(!ppu_memory_trace_append(journal, wanted[2]) && journal.count == 3);
    // One publication payload goes through the actual recorder as one event.
    const unsigned char publication[24] = {
        0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0x12, 0, 0, 0, 0x34, 0xa1, 0xb2, 0xc3, 0xd4};
    const PpuMemoryTraceEvent complete = ppu_memory_bytes_event(
        PpuMemoryTraceOrigin::Hle, true, 0x8eb1e0, publication, sizeof(publication));
    PpuMemoryTraceEvent publication_storage[1]{};
    PpuMemoryTraceRecorder publication_recorder;
    CHECK(publication_recorder.configure(publication_storage, 1, PpuMemoryTraceRetention::Strict));
    CHECK(publication_recorder.begin(1));
    publication_recorder.publish(complete);
    publication_recorder.finish();
    PpuMemoryTraceSample recorded{};
    CHECK(publication_recorder.read(0, recorded));
    CHECK(recorded.sequence == 0 && recorded.event.length == 24);
    CHECK(recorded.event.origin == PpuMemoryTraceOrigin::Hle && recorded.event.byte_transfer);
    CHECK(publication_recorder.report().published_count == 1);
    for (size_t index = 0; index < sizeof(publication); ++index)
        CHECK(index < sizeof(recorded.event.bytes) && recorded.event.bytes[index] == publication[index]);
    CHECK(recorded.event.operation == PpuMemoryTraceOperation::Write && recorded.event.address == 0x8eb1e0);
    CHECK(recorded.event.value == 0 && recorded.event.increment == 0 && !recorded.event.stored);
    CHECK(recorded.event.status == PS3_GUEST_MEMORY_OK);
    PpuMemoryTraceEvent changed_tail = complete;
    changed_tail.bytes[23] ^= 1;
    CHECK(!ppu_memory_event_equal(complete, changed_tail));
    CHECK(!ppu_memory_trace_equal(&complete, 1, &changed_tail, 1));
    const size_t lengths[] = {1, 16, 23, 24};
    for (size_t length : lengths) {
        const auto bytes = ppu_memory_bytes_event(PpuMemoryTraceOrigin::Generated, false, 0x10000, publication, length);
        CHECK(bytes.origin == PpuMemoryTraceOrigin::Generated && bytes.operation == PpuMemoryTraceOperation::Read);
        CHECK(bytes.length == length && bytes.byte_transfer && bytes.status == PS3_GUEST_MEMORY_OK);
        for (size_t index = 0; index < PPU_MEMORY_TRACE_BYTE_CAPACITY; ++index)
            CHECK(bytes.bytes[index] == (index < length ? publication[index] : 0));
    }
#ifndef __SWITCH__
    for (unsigned misuse = 0; misuse < 3; ++misuse) {
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            const rlimit no_core{0, 0};
            setrlimit(RLIMIT_CORE, &no_core);
            (void)ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true, 0x10000,
                misuse == 0 ? nullptr : publication, misuse == 1 ? 0 : PPU_MEMORY_TRACE_BYTE_CAPACITY + (misuse == 2));
            _exit(99);
        }
        if (child > 0) {
            int status = 0;
            CHECK(waitpid(child, &status, 0) == child);
            CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        }
    }
#endif
    std::printf("PPU_MEMORY_TRACE_%s failures=%d cross_class_swap_rejected=true byte_capacity=%zu event_size=%zu sample_size=%zu\n",
        failures ? "FAIL" : "PASS", failures, PPU_MEMORY_TRACE_BYTE_CAPACITY,
        sizeof(PpuMemoryTraceEvent), sizeof(PpuMemoryTraceSample));
    return failures != 0;
}

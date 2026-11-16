#include "../adapters/ppu_memory_trace.h"
extern "C" {
#include "guest_space_host.h"
#include "atomic_domain.h"
}
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int failures;
int cases;
#define CHECK(value) do { if (!(value)) { std::printf("TRACE_POLICY_FAIL line=%d\n", __LINE__); ++failures; } } while (0)

PpuMemoryTraceEvent event(PpuMemoryTraceOrigin origin, uint64_t value)
{
    return ppu_memory_scalar_event(origin, true, 0x1000, 8, value);
}

void expect(PpuMemoryTraceRecorder &recorder, size_t index, uint64_t sequence,
            PpuMemoryTraceOrigin origin, uint64_t value)
{
    PpuMemoryTraceSample sample{};
    CHECK(recorder.read(index, sample));
    CHECK(sample.sequence == sequence && sample.event.origin == origin);
    CHECK(sample.event.value == value && sample.event.operation == PpuMemoryTraceOperation::Write);
    CHECK(sample.event.status == PS3_GUEST_MEMORY_OK && !sample.event.stored);
}

void strict_and_configuration()
{
    ++cases;
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent storage[2]{};
    CHECK(!recorder.begin(1));
    CHECK(!recorder.configure(nullptr, 2, PpuMemoryTraceRetention::Suffix));
    CHECK(!recorder.configure(storage, 0, PpuMemoryTraceRetention::Suffix));
    CHECK(!recorder.configure(storage, 2, static_cast<PpuMemoryTraceRetention>(77)));
    CHECK(!recorder.configure(storage, std::numeric_limits<size_t>::max(), PpuMemoryTraceRetention::Suffix));
    const uintptr_t high = std::numeric_limits<uintptr_t>::max() & ~(uintptr_t(alignof(PpuMemoryTraceEvent)) - 1);
    CHECK(!recorder.configure(reinterpret_cast<PpuMemoryTraceEvent *>(high), 2, PpuMemoryTraceRetention::Suffix));
    CHECK(!recorder.report().configured);
    CHECK(recorder.configure(storage, 2, PpuMemoryTraceRetention::Strict));
    CHECK(!recorder.begin(0));
    CHECK(recorder.begin(2));
    CHECK(!recorder.begin(1));
    CHECK(!recorder.configure(storage, 2, PpuMemoryTraceRetention::Suffix));
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 11));
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 22));
    recorder.finish();
    const auto full = recorder.report();
    CHECK(full.configured && full.retained_count == 2 && full.published_count == 2);
    CHECK(full.retained_begin == 0 && full.retained_end == 2);
    CHECK(full.overwritten_begin == 0 && full.overwritten_end == 0);
    CHECK(!recorder.begin(1));
    expect(recorder, 0, 0, PpuMemoryTraceOrigin::Generated, 11);
    expect(recorder, 1, 1, PpuMemoryTraceOrigin::Hle, 22);
    PpuMemoryTraceSample untouched{};
    untouched.sequence = 999;
    untouched.event.value = 444;
    CHECK(!recorder.read(2, untouched));
    CHECK(untouched.sequence == 999 && untouched.event.value == 444);
    CHECK(!recorder.configure(nullptr, 1, PpuMemoryTraceRetention::Suffix));
    CHECK(recorder.report().published_count == full.published_count);
    expect(recorder, 0, 0, PpuMemoryTraceOrigin::Generated, 11);
}

void suffix_wrap()
{
    ++cases;
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent storage[3]{};
    CHECK(recorder.configure(storage, 3, PpuMemoryTraceRetention::Suffix));
    for (uint64_t sequence = 0; sequence < 8; ++sequence) {
        const bool admitted = recorder.begin(1);
        CHECK(admitted);
        if (!admitted) return;
        recorder.publish(event(sequence % 2 ? PpuMemoryTraceOrigin::Hle : PpuMemoryTraceOrigin::Generated,
                               100 + sequence));
        recorder.finish();
    }
    const auto report = recorder.report();
    CHECK(report.retained_count == 3 && report.published_count == 8);
    CHECK(report.retained_begin == 5 && report.retained_end == 8);
    CHECK(report.overwritten_begin == 0 && report.overwritten_end == 5);
    expect(recorder, 0, 5, PpuMemoryTraceOrigin::Hle, 105);
    expect(recorder, 1, 6, PpuMemoryTraceOrigin::Generated, 106);
    expect(recorder, 2, 7, PpuMemoryTraceOrigin::Hle, 107);
}

void one_entry_pair_and_partial_batch()
{
    ++cases;
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent storage[1]{};
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Strict));
    CHECK(!recorder.begin(2));
    CHECK(recorder.report().published_count == 0);
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Suffix));
    const bool admitted = recorder.begin(2);
    CHECK(admitted);
    if (!admitted) return;
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 31));
    expect(recorder, 0, 0, PpuMemoryTraceOrigin::Hle, 31);
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 32));
    recorder.finish();
    expect(recorder, 0, 1, PpuMemoryTraceOrigin::Hle, 32);
    CHECK(recorder.report().overwritten_end == 1);
    CHECK(recorder.begin(2));
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 33));
    recorder.finish(); // The unused publication is not a successful event or a sequence gap.
    CHECK(recorder.report().published_count == 3);
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 34));
    recorder.finish();
    expect(recorder, 0, 3, PpuMemoryTraceOrigin::Hle, 34);
    CHECK(recorder.report().overwritten_begin == 0 && recorder.report().overwritten_end == 3);
}

void overflow_and_independent_owners()
{
    ++cases;
    PpuMemoryTraceRecorder recorder, other;
    PpuMemoryTraceEvent storage[2]{}, other_storage[1]{};
    const uint64_t start = std::numeric_limits<uint64_t>::max() - 1;
    CHECK(recorder.configure(storage, 2, PpuMemoryTraceRetention::Suffix, start));
    CHECK(other.configure(other_storage, 1, PpuMemoryTraceRetention::Suffix));
    CHECK(!recorder.begin(2));
    CHECK(recorder.report().published_count == 0 && recorder.report().retained_begin == start);
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 41));
    recorder.finish();
    expect(recorder, 0, start, PpuMemoryTraceOrigin::Generated, 41);
    CHECK(!recorder.begin(1));
    CHECK(recorder.report().published_count == 1);
    CHECK(other.begin(1));
    other.publish(event(PpuMemoryTraceOrigin::Hle, 42));
    other.finish();
    expect(other, 0, 0, PpuMemoryTraceOrigin::Hle, 42);
    expect(recorder, 0, start, PpuMemoryTraceOrigin::Generated, 41);
}

void lock(void *context) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(context)); }
void unlock(void *context) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(context)); }

void real_memory_publication_pairs()
{
    ++cases;
    ElfTestHostSpace *space = elf_test_host_space_create(65536);
    CHECK(space != nullptr);
    if (!space) return;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_owner_record owners[2]{};
    ps3_atomic_pin_record pins[2]{};
    ps3_atomic_domain domain{};
    CHECK(ps3_atomic_domain_init(&domain, elf_test_host_space_port(space),
        ps3_instance_guard{&mutex, lock, unlock}, owners, 2, pins, 2) == PS3_GUEST_MEMORY_OK);
    const ps3_guest_memory memory = ps3_atomic_domain_memory(&domain);
    CHECK(memory.commit(memory.context, 0x1000, 4096) == PS3_GUEST_MEMORY_OK);
    const uint64_t original = 0;
    CHECK(memory.write(memory.context, 0x1000, &original, sizeof(original)) == PS3_GUEST_MEMORY_OK);
    PpuMemoryTraceEvent storage[1]{};
    PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Strict));
    const bool strict_admitted = recorder.begin(2);
    CHECK(!strict_admitted);
    uint64_t observed = 99;
    CHECK(memory.read(memory.context, 0x1000, &observed, sizeof(observed)) == PS3_GUEST_MEMORY_OK);
    CHECK(observed == 0);
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Suffix));
    const bool admitted = recorder.begin(2);
    CHECK(admitted);
    if (admitted) {
        const uint64_t output = 51, toc = 52;
        CHECK(memory.write(memory.context, 0x1000, &output, sizeof(output)) == PS3_GUEST_MEMORY_OK);
        recorder.publish(event(PpuMemoryTraceOrigin::Hle, 51));
        CHECK(memory.write(memory.context, 0x1008, &toc, sizeof(toc)) == PS3_GUEST_MEMORY_OK);
        recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 0x1008, 8, 52));
        recorder.finish();
        expect(recorder, 0, 1, PpuMemoryTraceOrigin::Hle, 52);
        CHECK(memory.read(memory.context, 0x1000, &observed, sizeof(observed)) == PS3_GUEST_MEMORY_OK && observed == 51);
        CHECK(memory.read(memory.context, 0x1008, &observed, sizeof(observed)) == PS3_GUEST_MEMORY_OK && observed == 52);
        CHECK(recorder.begin(1));
        const uint64_t generated = 53;
        CHECK(memory.write(memory.context, 0x1000, &generated, sizeof(generated)) == PS3_GUEST_MEMORY_OK);
        recorder.publish(event(PpuMemoryTraceOrigin::Generated, 53));
        recorder.finish();
        expect(recorder, 0, 2, PpuMemoryTraceOrigin::Generated, 53);
        CHECK(recorder.report().retained_count == 1 && recorder.report().published_count == 3);
        CHECK(recorder.report().overwritten_end == 2);
        // A failed real coherent access produces no successful publication.
        CHECK(recorder.begin(1));
        CHECK(memory.write(memory.context, 0x3000, &generated, sizeof(generated)) != PS3_GUEST_MEMORY_OK);
        recorder.finish();
        CHECK(recorder.report().published_count == 3);
    }
    CHECK(memory.decommit(memory.context, 0x1000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
    elf_test_host_space_destroy(space);
}

void partial_batch_lifecycle()
{
    ++cases;
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent storage[2]{}, replacement[1]{};
    CHECK(recorder.configure(storage, 2, PpuMemoryTraceRetention::Suffix, 100));
    CHECK(recorder.begin(2));
    CHECK(!recorder.begin(1));
    CHECK(!recorder.configure(replacement, 1, PpuMemoryTraceRetention::Strict));
    CHECK(recorder.report().batch_active && recorder.report().remaining_publications == 2);
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 61));
    CHECK(recorder.report().batch_active && recorder.report().remaining_publications == 1);
    CHECK(recorder.report().published_count == 1 && recorder.report().capacity == 2);
    expect(recorder, 0, 100, PpuMemoryTraceOrigin::Generated, 61);
    CHECK(!recorder.begin(1));
    CHECK(recorder.report().remaining_publications == 1);
    recorder.finish();
    recorder.finish();
    CHECK(!recorder.report().batch_active && recorder.report().remaining_publications == 0);
    CHECK(recorder.report().published_count == 1 && recorder.report().retained_end == 101);
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 62));
    recorder.finish();
    expect(recorder, 1, 101, PpuMemoryTraceOrigin::Hle, 62);
    const bool admitted = recorder.begin(2);
    CHECK(admitted);
    if (!admitted) return;
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 63));
    // Same quiescent state a joined owner can observe after a nonreturning worker stop.
    CHECK(recorder.report().batch_active && recorder.report().remaining_publications == 1);
    expect(recorder, 0, 101, PpuMemoryTraceOrigin::Hle, 62);
    expect(recorder, 1, 102, PpuMemoryTraceOrigin::Generated, 63);
    recorder.finish();
    CHECK(recorder.report().published_count == 3 && recorder.report().overwritten_end == 101);
    CHECK(recorder.report().overwritten_begin == 100 && recorder.report().retained_begin == 101 &&
          recorder.report().retained_end == 103);
}

struct PartialProducer {
    PpuMemoryTraceRecorder *recorder;
    bool admitted = false;
};

void *publish_partial(void *opaque)
{
    auto *producer = static_cast<PartialProducer *>(opaque);
    producer->admitted = producer->recorder->begin(2);
    if (producer->admitted)
        producer->recorder->publish(event(PpuMemoryTraceOrigin::Hle, 81));
    return nullptr; // No implicit finish: root must inspect/finalize after join.
}

void joined_owner_finishes_partial_batch()
{
    ++cases;
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent storage[1]{};
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Suffix));
    PartialProducer producer{&recorder};
    pthread_t worker;
    const int created = pthread_create(&worker, nullptr, publish_partial, &producer);
    CHECK(created == 0);
    if (created != 0) return;
    const int joined = pthread_join(worker, nullptr);
    CHECK(joined == 0);
    if (joined != 0) std::abort(); // Do not release borrowed storage of an unjoined producer.
    CHECK(producer.admitted);
    if (!producer.admitted) return;
    CHECK(recorder.report().batch_active && recorder.report().remaining_publications == 1);
    expect(recorder, 0, 0, PpuMemoryTraceOrigin::Hle, 81);
    recorder.finish();
    CHECK(!recorder.report().batch_active && recorder.report().published_count == 1);
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 82));
    recorder.finish();
    expect(recorder, 0, 1, PpuMemoryTraceOrigin::Generated, 82);
}

bool same_report(const PpuMemoryTraceReport &left, const PpuMemoryTraceReport &right)
{
    return left.configured == right.configured && left.capacity == right.capacity &&
        left.retained_count == right.retained_count && left.published_count == right.published_count &&
        left.retained_begin == right.retained_begin && left.retained_end == right.retained_end &&
        left.overwritten_begin == right.overwritten_begin && left.overwritten_end == right.overwritten_end &&
        left.batch_active == right.batch_active && left.remaining_publications == right.remaining_publications;
}

void query_unchanged(const PpuMemoryTraceRecorder &recorder, const PpuMemoryTraceEvent *storage,
                     size_t capacity, size_t requested, bool wanted)
{
    unsigned char before[sizeof(PpuMemoryTraceEvent) * 2];
    CHECK(capacity <= 2);
    std::memcpy(before, storage, capacity * sizeof(*storage));
    const auto report = recorder.report();
    CHECK(recorder.can_begin(requested) == wanted);
    CHECK(same_report(report, recorder.report()));
    CHECK(std::memcmp(before, storage, capacity * sizeof(*storage)) == 0);
}

void pure_admission_queries()
{
    ++cases;
    PpuMemoryTraceEvent storage[2]{};
    storage[0] = event(PpuMemoryTraceOrigin::Generated, 91);
    PpuMemoryTraceRecorder recorder;
    query_unchanged(recorder, storage, 2, 0, false);
    query_unchanged(recorder, storage, 2, 1, false);
    CHECK(recorder.configure(storage, 2, PpuMemoryTraceRetention::Strict));
    query_unchanged(recorder, storage, 2, 0, false);
    for (int repetition = 0; repetition < 3; ++repetition)
        query_unchanged(recorder, storage, 2, 2, true);
    CHECK(!recorder.report().batch_active && recorder.report().remaining_publications == 0);
    CHECK(recorder.begin(2));
    query_unchanged(recorder, storage, 2, 1, false);
    CHECK(recorder.report().remaining_publications == 2);
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 92));
    query_unchanged(recorder, storage, 2, 1, false);
    CHECK(recorder.report().remaining_publications == 1);
    recorder.finish();
    query_unchanged(recorder, storage, 2, 1, true);
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 93));
    recorder.finish();
    for (int repetition = 0; repetition < 3; ++repetition)
        query_unchanged(recorder, storage, 2, 1, false);
    recorder.finish();
    CHECK(!recorder.begin(1));
    CHECK(recorder.report().published_count == 2 && recorder.report().retained_count == 2);
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Suffix));
    for (int repetition = 0; repetition < 3; ++repetition)
        query_unchanged(recorder, storage, 1, 2, true);
    CHECK(recorder.begin(2));
    CHECK(recorder.report().remaining_publications == 2);
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 94));
    query_unchanged(recorder, storage, 1, 2, false);
    CHECK(recorder.report().remaining_publications == 1);
    recorder.finish();
    query_unchanged(recorder, storage, 1, 2, true);
    CHECK(recorder.report().published_count == 1);
    const uint64_t start = std::numeric_limits<uint64_t>::max() - 1;
    CHECK(recorder.configure(storage, 1, PpuMemoryTraceRetention::Suffix, start));
    for (int repetition = 0; repetition < 3; ++repetition) {
        query_unchanged(recorder, storage, 1, 2, false);
        query_unchanged(recorder, storage, 1, 1, true);
    }
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Generated, 95));
    recorder.finish();
    for (int repetition = 0; repetition < 3; ++repetition)
        query_unchanged(recorder, storage, 1, 1, false);
    recorder.finish();
    CHECK(!recorder.begin(1));
    CHECK(recorder.report().published_count == 1 && recorder.report().retained_end == UINT64_MAX);
    expect(recorder, 0, start, PpuMemoryTraceOrigin::Generated, 95);
}

void misaligned_configuration_preserves_state()
{
    ++cases; // Regression coverage: configure already rejects misalignment.
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent storage[2]{};
    CHECK(recorder.configure(storage, 2, PpuMemoryTraceRetention::Suffix, 200));
    CHECK(recorder.begin(1));
    recorder.publish(event(PpuMemoryTraceOrigin::Hle, 96));
    recorder.finish();
    const auto before = recorder.report();
    unsigned char bytes[sizeof(storage)];
    std::memcpy(bytes, storage, sizeof(storage));
    auto *misaligned = reinterpret_cast<PpuMemoryTraceEvent *>(reinterpret_cast<uintptr_t>(storage) + 1);
    CHECK(!recorder.configure(misaligned, 1, PpuMemoryTraceRetention::Strict));
    CHECK(same_report(before, recorder.report()));
    CHECK(std::memcmp(bytes, storage, sizeof(storage)) == 0);
    expect(recorder, 0, 200, PpuMemoryTraceOrigin::Hle, 96);
    query_unchanged(recorder, storage, 2, 3, true); // Failed configure must preserve Suffix policy.
}

void misuse_aborts_in_release()
{
    ++cases;
#ifndef NDEBUG
#error "Misuse contract must be exercised with NDEBUG"
#endif
    for (int scenario = 0; scenario < 4; ++scenario) {
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child < 0) continue;
        if (child == 0) {
            const rlimit no_core{0, 0};
            setrlimit(RLIMIT_CORE, &no_core);
            PpuMemoryTraceRecorder recorder;
            PpuMemoryTraceEvent storage[1]{};
            if (scenario != 0 && !recorder.configure(storage, 1, PpuMemoryTraceRetention::Suffix)) _exit(20);
            if (scenario >= 2) {
                if (!recorder.begin(1)) _exit(21);
                if (scenario == 2) recorder.publish(event(PpuMemoryTraceOrigin::Hle, 70));
                else recorder.finish();
            }
            recorder.publish(event(PpuMemoryTraceOrigin::Generated, 71));
            _exit(22);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}
}

int main()
{
    strict_and_configuration();
    suffix_wrap();
    one_entry_pair_and_partial_batch();
    overflow_and_independent_owners();
    real_memory_publication_pairs();
    partial_batch_lifecycle();
    misuse_aborts_in_release();
    joined_owner_finishes_partial_batch();
    pure_admission_queries();
    misaligned_configuration_preserves_state();
    std::printf("PPU_MEMORY_TRACE_POLICY_%s cases=%d failures=%d real_domain=true\n",
        failures ? "FAIL" : "PASS", cases, failures);
    return failures != 0;
}

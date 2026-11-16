/* Host bridge fixture: this includes the real bridge to establish its worker
 * boundary directly. It does not execute a native generated import stub. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>

namespace {
int failures, cases;
#define CHECK(value) do { if (!(value)) { std::printf("STARTUP_RECORDER_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
struct Terminal {};
PpuMemoryTraceRecorder *terminal_recorder;
pthread_mutex_t *terminal_guard;
pthread_mutex_t *terminal_mutex_guard, *terminal_condition_guard;
bool terminal_closed, terminal_unlocked;
uint64_t host_tick;
void guard_lock(void *p) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(p)); }
void guard_unlock(void *p) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p)); }
struct MemoryProbe {
    ps3_guest_memory backing{};
    StartupControl *control = nullptr;
    bool cancel_output = false, fail_toc = false, cancel_read = false;
    unsigned writes = 0, reads = 0;
    bool fail_condition_create_write = false;
    unsigned failed_condition_writes = 0;
    static ps3_guest_memory_status commit(void *p, uint32_t address, uint64_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        return self.backing.commit(self.backing.context, address, length);
    }
    static ps3_guest_memory_status decommit(void *p, uint32_t address, uint64_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        return self.backing.decommit(self.backing.context, address, length);
    }
    static ps3_guest_memory_status read(void *p, uint32_t address, void *bytes, size_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        ++self.reads;
        const auto status = self.backing.read(self.backing.context, address, bytes, length);
        if (self.cancel_read && address == 0x10200 && status == PS3_GUEST_MEMORY_OK)
            self.control->cancelled.store(true);
        return status;
    }
    static ps3_guest_memory_status write(void *p, uint32_t address, const void *bytes, size_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        if (self.fail_condition_create_write && address == 0x10200 && length == 8) {
            ++self.failed_condition_writes;
            return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        }
        if (self.fail_toc && address == 0x10128) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto status = self.backing.write(self.backing.context, address, bytes, length);
        if (status == PS3_GUEST_MEMORY_OK) {
            ++self.writes;
            if (self.cancel_output && address == 0x10200) self.control->cancelled.store(true);
        }
        return status;
    }
    static ps3_guest_memory_status zero(void *p, uint32_t address, uint64_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        return self.backing.zero(self.backing.context, address, length);
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};
struct Runtime {
    ElfTestHostSpace *space = elf_test_host_space_create(0x30000);
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[4]{};
    ps3_atomic_pin_record pins[4]{};
    MemoryProbe probe{};
    PpuMemoryRamRange ram{0x10000, 8192, true};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[4]{};
    ps3_atomic_domain &initialize() {
        if (!space) std::abort();
        probe.backing = elf_test_host_space_port(space);
        if (ps3_atomic_domain_init(&domain, probe.port(),
            {&guard, guard_lock, guard_unlock}, owners, 4, pins, 4) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    Runtime() : binding(initialize(), &ram, 1) {
        CHECK(ps3_atomic_commit(&domain, ram.address, ram.length) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        execution.control = control.get();
        probe.control = control.get();
        execution.ppu.cpu.gpr[1] = 0x10100;
        execution.ppu.cpu.gpr[2] = UINT64_C(0x123456789abcdef0);
        execution.ppu.cpu.gpr[3] = 0x10200;
        execution.ppu.cpu.thread_id = 0x1234;
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(pthread_mutex_destroy(&guard) == 0);
        elf_test_host_space_destroy(space);
    }
    uint64_t bytes(uint32_t address) {
        unsigned char value[8]{};
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, address, value, 8) == PS3_GUEST_MEMORY_OK);
        uint64_t result = 0; for (unsigned char byte : value) result = result * 256 + byte;
        return result;
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return host_tick; }
extern "C" [[noreturn]] void threadExit(void) {
    terminal_closed = !terminal_recorder || !terminal_recorder->report().batch_active;
    const int result = pthread_mutex_trylock(terminal_guard);
    terminal_unlocked = result == 0;
    if (!result) pthread_mutex_unlock(terminal_guard);
    CHECK(terminal_closed); CHECK(terminal_unlocked);
    pthread_mutex_t *guards[] = {terminal_mutex_guard, terminal_condition_guard};
    for (auto *guard : guards) {
        if (!guard) continue;
        const int status = pthread_mutex_trylock(guard);
        CHECK(status == 0);
        if (!status) pthread_mutex_unlock(guard);
    }
    throw Terminal{}; // Observations above precede stack unwinding.
}
#include <cerrno>
#include <ctime>
#include <initializer_list>
namespace {
struct ConditionWait {
    pthread_cond_t condition{};
    ConditionWait() {
        pthread_condattr_t attr;
        CHECK(!pthread_condattr_init(&attr));
        CHECK(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
        CHECK(!pthread_cond_init(&condition, &attr));
        CHECK(!pthread_condattr_destroy(&attr));
    }
    ~ConditionWait() { CHECK(!pthread_cond_destroy(&condition)); }
    static ps3_wait_status now(void *, uint64_t *output) {
        timespec value{};
        if (clock_gettime(CLOCK_MONOTONIC, &value)) std::abort();
        *output = uint64_t(value.tv_sec) * UINT64_C(1000000000) + uint64_t(value.tv_nsec);
        return PS3_WAIT_READY;
    }
    static ps3_wait_status park(void *context, const ps3_instance_guard *guard,
        uint64_t deadline, ps3_wait_cancel cancel) {
        if (cancel.is_cancelled && cancel.is_cancelled(cancel.context)) return PS3_WAIT_CANCELLED;
        uint64_t value = 0; now(context, &value);
        const uint64_t tick = deadline < value + 2000000 ? deadline : value + 2000000;
        const timespec until{time_t(tick / 1000000000), long(tick % 1000000000)};
        const int status = pthread_cond_timedwait(&static_cast<ConditionWait *>(context)->condition,
            static_cast<pthread_mutex_t *>(guard->context), &until);
        if (status && status != ETIMEDOUT) std::abort();
        return PS3_WAIT_READY;
    }
    static void notify(void *context) {
        if (pthread_cond_broadcast(&static_cast<ConditionWait *>(context)->condition)) std::abort();
    }
    ps3_wait_port port() { return {this, now, park, notify}; }
};
struct ConditionRuntime : Runtime {
    pthread_mutex_t mutex_guard = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t condition_guard = PTHREAD_MUTEX_INITIALIZER;
    ConditionWait mutex_wait, condition_wait;
    ps3_lwmutex_registry mutexes{};
    ps3_lwmutex_object mutex_objects[2]{};
    ps3_lwmutex_waiter mutex_waiters[4]{};
    ps3_lwcond_registry conditions{};
    ps3_lwcond_object condition_objects[2]{};
    ps3_lwcond_waiter condition_waiters[2]{};
    PpuMemoryTraceEvent retained[16]{};
    static ps3_lwcond_thread_status exists(void *, uint32_t id, unsigned *output) {
        *output = id == 0x1234 || id == 0x2345;
        return PS3_LWCOND_THREAD_OK;
    }
    ConditionRuntime() {
        CHECK(ps3_lwmutex_init_with_conditions(&mutexes, &domain,
            {&mutex_guard, guard_lock, guard_unlock}, mutex_wait.port(),
            mutex_objects, 2, mutex_waiters, 4, 2).status == PS3_LWMUTEX_OK);
        CHECK(ps3_lwmutex_create(&mutexes, 0x10400, PS3_LWMUTEX_FIFO,
            PS3_LWMUTEX_RECURSIVE).status == PS3_LWMUTEX_OK);
        CHECK(ps3_lwcond_init(&conditions, &mutexes,
            {&condition_guard, guard_lock, guard_unlock}, condition_wait.port(),
            {nullptr, exists}, condition_objects, 2, condition_waiters, 2).status == PS3_LWCOND_OK);
        execution.conditions = &conditions;
        execution.has_guest_priority = true;
        execution.guest_priority = 250;
        CHECK(recorder.configure(retained, 16, PpuMemoryTraceRetention::Strict, 41));
        CHECK(native_startup_admit_recorded(execution, binding, services, recorder));
        const unsigned char name[8] = {'a', 0, 'b', 0xff, 1, 2, 3, 4};
        CHECK(ps3_atomic_write(&domain, 0x10500, name, 8) == PS3_GUEST_MEMORY_OK);
        const unsigned char sentinel[8] = {0x91,0x82,0x73,0x64,0x55,0x46,0x37,0x28};
        CHECK(ps3_atomic_write(&domain, 0x10200, sentinel, 8) == PS3_GUEST_MEMORY_OK);
        CHECK(ps3_atomic_write(&domain, 0x10128, sentinel, 8) == PS3_GUEST_MEMORY_OK);
        probe.writes = probe.reads = 0;
    }
    ~ConditionRuntime() {
        ps3_lwcond_report report{};
        if (ps3_lwcond_cleanup(&conditions, &report).status != PS3_LWCOND_OK) std::abort();
        if (ps3_lwmutex_cleanup(&mutexes).status != PS3_LWMUTEX_OK) std::abort();
        CHECK(!pthread_mutex_destroy(&condition_guard));
        CHECK(!pthread_mutex_destroy(&mutex_guard));
    }
    void call(uint32_t nid, uint64_t address = 0x10200) {
        execution.ppu.cpu.gpr[3] = address;
        const ppu_context before = execution.ppu.cpu;
        terminal_recorder = &recorder; terminal_guard = &guard;
        terminal_mutex_guard = &mutex_guard; terminal_condition_guard = &condition_guard;
        terminal_closed = terminal_unlocked = false;
        active = control.get();
        try { ps3_hle_call(nid, &execution.ppu.cpu); active = nullptr; }
        catch (const Terminal &) {
            CHECK(terminal_closed && terminal_unlocked);
            CHECK(!pthread_mutex_trylock(&mutex_guard)); pthread_mutex_unlock(&mutex_guard);
            CHECK(!pthread_mutex_trylock(&condition_guard)); pthread_mutex_unlock(&condition_guard);
        }
        ppu_context expected = before;
        if (control->stop == StartupStop::None) expected.gpr[3] = execution.ppu.cpu.gpr[3];
        CHECK(!std::memcmp(&expected, &execution.ppu.cpu, sizeof(expected)));
        std::printf("LWCOND_HOST nid=0x%x stop=%d result=%d memory=%d wait=%d mutex=%d publications=%zu selected=%zu\n",
            nid, int(control->stop), int(control->condition_result.status),
            int(control->condition_result.memory_status), int(control->condition_result.wait_status),
            int(control->condition_result.mutex_status), control->condition_publications,
            control->condition_selected);
        terminal_recorder = nullptr; terminal_guard = nullptr;
        terminal_mutex_guard = terminal_condition_guard = nullptr;
    }
    void create() {
        execution.ppu.cpu.gpr[4] = 0x10400;
        execution.ppu.cpu.gpr[5] = 0x10500;
        call(0xda0eb71a);
    }
};
[[maybe_unused]] void check_publication(ConditionRuntime &r, size_t index, uint64_t sequence,
    uint32_t address, const unsigned char *bytes, size_t length) {
    PpuMemoryTraceSample sample{};
    CHECK(r.recorder.read(index, sample));
    CHECK(sample.sequence == sequence && sample.event.origin == PpuMemoryTraceOrigin::Hle);
    CHECK(sample.event.operation == PpuMemoryTraceOperation::Write);
    CHECK(sample.event.address == address && sample.event.length == length);
    CHECK(sample.event.status == PS3_GUEST_MEMORY_OK && sample.event.byte_transfer && !sample.event.stored);
    CHECK(sample.event.value == 0 && !std::memcmp(sample.event.bytes, bytes, length));
}
void create_destroy() {
    ++cases; ConditionRuntime r;
#ifdef PS3_NATIVE_STARTUP_LWCOND
    auto before = r.execution.ppu.cpu;
    before.gpr[4] = 0x10400; before.gpr[5] = 0x10500; before.gpr[3] = 0;
#endif
    r.create();
#ifdef PS3_NATIVE_STARTUP_LWCOND
    CHECK(r.control->stop == StartupStop::None);
    CHECK(!std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)));
    CHECK(r.bytes(0x10200) == UINT64_C(0x0001040000000001));
    CHECK(r.recorder.report().published_count == 2);
    const unsigned char created[] = {0,1,4,0,0,0,0,1};
    check_publication(r, 0, 41, 0x10200, created, 8);
    PpuMemoryTraceSample toc{}; CHECK(r.recorder.read(1, toc));
    CHECK(toc.sequence == 42 && toc.event.address == 0x10128 && toc.event.length == 8);
    CHECK(toc.event.origin == PpuMemoryTraceOrigin::Hle && toc.event.operation == PpuMemoryTraceOperation::Write);
    CHECK(!toc.event.byte_transfer && !toc.event.stored && toc.event.status == PS3_GUEST_MEMORY_OK);
    CHECK(toc.event.value == UINT64_C(0x123456789abcdef0));
    r.call(0x1c9a942c);
    CHECK(r.execution.ppu.cpu.gpr[3] == 0);
    CHECK(r.bytes(0x10200) == UINT64_C(0x00010400fffffffe));
#else
    CHECK(r.control->stop == StartupStop::UnknownImport);
    CHECK(r.bytes(0x10200) == UINT64_C(0x9182736455463728) && r.recorder.report().published_count == 0);
#endif
}
#ifdef PS3_NATIVE_STARTUP_LWCOND
void direct_create(ConditionRuntime &r) {
    const unsigned char name[8] = {0}; ps3_lwcond_report report{};
    CHECK(ps3_lwcond_create(&r.conditions, 0x10200, 0x10400, name, &report).status == PS3_LWCOND_OK);
}
void family_errors() {
    for (uint32_t nid : {UINT32_C(0xef87a695), UINT32_C(0xe9a1bd84), UINT32_C(0x52aadadf), UINT32_C(0x1c9a942c)}) {
        ++cases; ConditionRuntime r; r.execution.ppu.cpu.gpr[4] = 0x2345;
        r.call(nid);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.execution.ppu.cpu.gpr[3] == UINT64_C(0xffffffff80010005));
        CHECK(r.recorder.report().published_count == 1);
    }
    for (uint64_t target : {UINT64_C(0x2345), UINT64_C(0x3456)}) {
        ++cases; ConditionRuntime r; direct_create(r);
        r.execution.ppu.cpu.gpr[4] = target;
        r.call(0x52aadadf);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.execution.ppu.cpu.gpr[3] == (target == 0x2345 ? UINT64_C(0xffffffff80010009) : UINT64_C(0xffffffff80010005)));
    }
}
void wait_timeout_and_owner() {
    for (bool owned : {false, true}) {
        ++cases; ConditionRuntime r; direct_create(r);
        if (owned) {
            CHECK(ps3_lwmutex_try_lock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK);
            CHECK(ps3_lwmutex_try_lock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK);
        }
        r.execution.ppu.cpu.gpr[4] = 1000;
        r.call(0x2a6d9d51);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.execution.ppu.cpu.gpr[3] == (owned ? UINT64_C(0xffffffff8001000b) : UINT64_C(0xffffffff80010009)));
        if (owned) {
            CHECK(r.control->condition_result.mutex_reacquired == 1);
            const unsigned char released[24] = {255,255,255,255,0,0,0,0,0,0,0,17,0,0,0,0,0,0,0,1,0,0,0,0};
            const unsigned char promoted[24] = {255,255,255,255,0,0,0,1,0,0,0,17,0,0,0,0,0,0,0,1,0,0,0,0};
            const unsigned char restored[24] = {0,0,18,52,0,0,0,0,0,0,0,17,0,0,0,1,0,0,0,1,0,0,0,0};
            check_publication(r, 0, 41, 0x10400, released, 24);
            check_publication(r, 1, 42, 0x10400, promoted, 24);
            check_publication(r, 2, 43, 0x10400, restored, 24);
            CHECK(ps3_lwmutex_unlock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK);
            CHECK(ps3_lwmutex_unlock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK);
        }
    }
}
void signal_empty() {
    for (uint32_t nid : {UINT32_C(0xef87a695), UINT32_C(0xe9a1bd84)}) {
        ++cases; ConditionRuntime r; direct_create(r); r.call(nid);
        CHECK(r.control->stop == StartupStop::None && r.execution.ppu.cpu.gpr[3] == 0);
        CHECK(r.control->condition_selected == 0 && r.control->condition_publications == 0);
        CHECK(r.recorder.report().published_count == 1);
    }
}
void *cancel_wait_worker(void *context) {
    auto &r = *static_cast<ConditionRuntime *>(context);
    bool queued = false;
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        pthread_mutex_lock(&r.condition_guard);
        queued = r.condition_objects[0].active_waiters != 0;
        pthread_mutex_unlock(&r.condition_guard);
        if (queued) break;
        const timespec pause{0,1000000}; nanosleep(&pause, nullptr);
    }
    if (!queued) std::abort();
    r.control->cancelled.store(true, std::memory_order_release);
    ConditionWait::notify(&r.condition_wait);
    return nullptr;
}
void stopped_wait() {
    ++cases; ConditionRuntime r; direct_create(r);
    CHECK(ps3_lwmutex_try_lock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK);
    pthread_t worker; CHECK(!pthread_create(&worker, nullptr, cancel_wait_worker, &r));
    r.execution.ppu.cpu.gpr[4] = 0;
    r.call(0x2a6d9d51);
    const timespec until = [] { timespec value{}; clock_gettime(CLOCK_REALTIME, &value); value.tv_sec += 3; return value; }();
    if (pthread_timedjoin_np(worker, nullptr, &until)) std::abort();
    CHECK(r.control->stop == StartupStop::Cancelled);
    CHECK(r.control->condition_result.must_not_resume && r.control->condition_result.cleanup_retained);
    CHECK(!r.recorder.report().batch_active && r.recorder.report().published_count == 1);
    CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
}
void partial_create(bool fail_toc) {
    ++cases; ConditionRuntime r;
    r.probe.cancel_output = !fail_toc; r.probe.fail_toc = fail_toc;
    r.execution.ppu.cpu.gpr[4] = 0x10400; r.execution.ppu.cpu.gpr[5] = 0x10500;
    const auto before = r.execution.ppu.cpu;
    r.create();
    CHECK(r.control->stop == (fail_toc ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    CHECK(!std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)));
    CHECK(r.bytes(0x10200) == UINT64_C(0x0001040000000001));
    CHECK(r.recorder.report().published_count == 1 && !r.recorder.report().batch_active);
    const unsigned char created[] = {0,1,4,0,0,0,0,1};
    check_publication(r, 0, 41, 0x10200, created, 8);
    CHECK(r.control->condition_result.status == PS3_LWCOND_OK && r.control->condition_returns == 0);
}
void duplicate_and_unsupported_target() {
    { ++cases; ConditionRuntime r; direct_create(r); r.create();
      CHECK(r.control->stop == StartupStop::ConditionProviderFailure);
      CHECK(r.control->condition_result.status == PS3_LWCOND_UNSUPPORTED); }
    { ++cases; ConditionRuntime r; direct_create(r); r.execution.ppu.cpu.gpr[4] = UINT64_C(0x100002345);
      r.call(0x52aadadf); CHECK(r.control->stop == StartupStop::ConditionProviderFailure);
      CHECK(r.control->condition_result.status == PS3_LWCOND_UNSUPPORTED); }
}
void toc_control_overlap() {
    for (uint32_t nid : {UINT32_C(0xef87a695), UINT32_C(0xe9a1bd84), UINT32_C(0x52aadadf),
                        UINT32_C(0x1c9a942c), UINT32_C(0x2a6d9d51), UINT32_C(0xda0eb71a)}) {
        ++cases; ConditionRuntime r;
        r.execution.ppu.cpu.gpr[4] = 0x10400; r.execution.ppu.cpu.gpr[5] = 0x10500;
        r.call(nid, 0x10128);
        CHECK(r.control->stop == StartupStop::InvalidMemory && r.control->condition_calls == 0);
        CHECK(!r.probe.writes && !r.recorder.report().published_count);
        CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
    }
    { ++cases; ConditionRuntime r;
      r.execution.ppu.cpu.gpr[4] = 0x10120; r.execution.ppu.cpu.gpr[5] = 0x10500;
      r.call(0xda0eb71a); CHECK(r.control->stop == StartupStop::InvalidMemory);
      CHECK(r.control->condition_calls == 0 && !r.probe.writes); }
    { ++cases; ConditionRuntime r;
      r.execution.ppu.cpu.gpr[4] = 0x10400; r.execution.ppu.cpu.gpr[5] = 0x10128;
      r.call(0xda0eb71a); CHECK(r.control->stop == StartupStop::None);
      const unsigned char expected[] = {0x91,0x82,0x73,0x64,0x55,0x46,0x37,0x28};
      CHECK(!std::memcmp(r.condition_objects[0].name, expected, 8)); }
    { ++cases; ConditionRuntime r; direct_create(r);
      CHECK(ps3_lwmutex_try_lock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK);
      r.execution.ppu.cpu.gpr[1] = 0x103d8; r.execution.ppu.cpu.gpr[4] = 1000;
      r.probe.writes = 0;
      r.call(0x2a6d9d51);
      CHECK(r.control->stop == StartupStop::InvalidMemory);
      CHECK(r.control->condition_result.status == PS3_LWCOND_TIMED_OUT);
      CHECK(r.control->condition_result.mutex_reacquired == 1);
      CHECK(r.recorder.report().published_count == 3 && r.probe.writes == 3);
      CHECK(!r.recorder.report().batch_active && r.bytes(0x10400) == UINT64_C(0x0000123400000000));
      CHECK(ps3_lwmutex_unlock(&r.mutexes, 0x10400, {0x1234, 250}).status == PS3_LWMUTEX_OK); }
}
struct BusyWaitWorker {
    ConditionRuntime *runtime;
    std::atomic<bool> stop{false};
    ps3_lwcond_result result{};
    static int stopped(void *context) {
        return static_cast<BusyWaitWorker *>(context)->stop.load(std::memory_order_acquire);
    }
    static void *run(void *context) {
        auto &self = *static_cast<BusyWaitWorker *>(context);
        auto &r = *self.runtime;
        if (ps3_lwmutex_try_lock(&r.mutexes, 0x10400, {0x2345,250}).status != PS3_LWMUTEX_OK) std::abort();
        ps3_lwcond_report report{};
        self.result = ps3_lwcond_wait(&r.conditions, 0x10200, {0x2345,250}, 0, {},
            {&self, stopped}, &report);
        return nullptr;
    }
};
void busy_destroy() {
    ++cases; ConditionRuntime r; direct_create(r);
    BusyWaitWorker worker{&r}; pthread_t thread;
    CHECK(!pthread_create(&thread, nullptr, BusyWaitWorker::run, &worker));
    bool queued = false;
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        pthread_mutex_lock(&r.condition_guard);
        queued = r.condition_objects[0].active_waiters != 0;
        pthread_mutex_unlock(&r.condition_guard);
        if (queued) break;
        const timespec pause{0,1000000}; nanosleep(&pause, nullptr);
    }
    if (!queued) std::abort();
    r.call(0x1c9a942c);
    CHECK(r.control->stop == StartupStop::None && r.execution.ppu.cpu.gpr[3] == UINT64_C(0xffffffff8001000a));
    CHECK(r.control->condition_result.status == PS3_LWCOND_BUSY && r.control->condition_publications == 0);
    CHECK(r.recorder.report().published_count == 1 && r.bytes(0x10200) == UINT64_C(0x0001040000000001));
    worker.stop.store(true, std::memory_order_release); ConditionWait::notify(&r.condition_wait);
    timespec until{}; if (clock_gettime(CLOCK_REALTIME, &until)) std::abort(); until.tv_sec += 3;
    if (pthread_timedjoin_np(thread, nullptr, &until)) std::abort();
    CHECK(worker.result.must_not_resume && worker.result.cleanup_retained);
}
void failed_create_publication() {
    ++cases; ConditionRuntime r;
    r.execution.ppu.cpu.gpr[4] = 0x10400; r.execution.ppu.cpu.gpr[5] = 0x10500;
    const auto before = r.execution.ppu.cpu;
    r.probe.fail_condition_create_write = true;
    r.create();
    CHECK(r.control->stop == StartupStop::ConditionProviderFailure);
    CHECK(r.control->condition_calls == 1 && r.control->condition_returns == 0);
    CHECK(r.control->condition_result.status == PS3_LWCOND_MEMORY_FAILURE);
    CHECK(r.control->condition_result.memory_status == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    CHECK(r.probe.failed_condition_writes == 1 && r.probe.writes == 0);
    CHECK(!std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)));
    CHECK(r.bytes(0x10200) == UINT64_C(0x9182736455463728));
    CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
    CHECK(r.recorder.report().published_count == 0 && !r.recorder.report().batch_active);
    CHECK(r.recorder.report().remaining_publications == 0);
    CHECK(!r.condition_objects[0].state && !r.condition_objects[0].has_pin);
    CHECK(!r.mutex_objects[0].condition_bindings);
    r.probe.fail_condition_create_write = false;
    // The exact failed creation did not retain a resource: real retry succeeds.
    r.control->stop = StartupStop::None;
    r.create();
    CHECK(r.control->stop == StartupStop::None && r.execution.ppu.cpu.gpr[3] == 0);
}
void exhausted_condition_registry() {
    ++cases; ConditionRuntime r;
    const unsigned char name[8] = {0}; ps3_lwcond_report report{};
    CHECK(ps3_lwcond_create(&r.conditions, 0x10200, 0x10400, name, &report).status == PS3_LWCOND_OK);
    report = {};
    CHECK(ps3_lwcond_create(&r.conditions, 0x10208, 0x10400, name, &report).status == PS3_LWCOND_OK);
    const uint64_t first = r.bytes(0x10200), second = r.bytes(0x10208);
    CHECK(first == UINT64_C(0x0001040000000001) && second == UINT64_C(0x0001040000000002));
    r.probe.writes = 0;
    r.execution.ppu.cpu.gpr[4] = 0x10400; r.execution.ppu.cpu.gpr[5] = 0x10500;
    r.call(0xda0eb71a, 0x10210);
    CHECK(r.control->stop == StartupStop::None);
    CHECK(r.execution.ppu.cpu.gpr[3] == UINT64_C(0xffffffff80010001));
    CHECK(r.control->condition_result.status == PS3_LWCOND_RESOURCE_EXHAUSTED);
    CHECK(r.control->condition_publications == 0 && r.control->condition_returns == 1);
    CHECK(r.probe.writes == 1 && r.recorder.report().published_count == 1);
    CHECK(r.bytes(0x10200) == first && r.bytes(0x10208) == second && r.bytes(0x10210) == 0);
    PpuMemoryTraceSample sample{}; CHECK(r.recorder.read(0, sample));
    CHECK(sample.sequence == 41 && sample.event.origin == PpuMemoryTraceOrigin::Hle);
    CHECK(sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == 0x10128);
    CHECK(sample.event.length == 8 && sample.event.value == UINT64_C(0x123456789abcdef0));
    CHECK(!sample.event.byte_transfer && !sample.event.stored && sample.event.status == PS3_GUEST_MEMORY_OK);
}
void prepublication_failures() {
    for (uint64_t address : {UINT64_C(0), UINT64_C(0x100010200), UINT64_C(0xfffffffc)}) {
        ++cases; ConditionRuntime r;
        r.call(0xef87a695, address);
        CHECK(r.control->stop == StartupStop::InvalidMemory);
        CHECK(!r.probe.writes && !r.recorder.report().published_count);
    }
    { ++cases; ConditionRuntime r; r.execution.conditions = nullptr;
      r.call(0xef87a695); CHECK(r.control->stop == StartupStop::MissingServices); }
    { ++cases; ConditionRuntime r; r.probe.cancel_read = true;
      r.create(); CHECK(r.control->stop == StartupStop::Cancelled);
      CHECK(r.probe.reads == 2 && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ConditionRuntime r; r.control->cancelled.store(true);
      r.create(); CHECK(r.control->stop == StartupStop::Cancelled);
      CHECK(!r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ConditionRuntime r; r.recorder.finish();
      CHECK(r.recorder.configure(r.retained, 1, PpuMemoryTraceRetention::Strict));
      r.create(); CHECK(r.control->stop == StartupStop::TraceCapacity);
      CHECK(!r.probe.writes && !r.recorder.report().published_count); }
}
#endif
}
int main() {
    failures = cases = 0; host_tick = 0;
    create_destroy();
#ifdef PS3_NATIVE_STARTUP_LWCOND
    family_errors(); wait_timeout_and_owner(); prepublication_failures();
    partial_create(false); partial_create(true); duplicate_and_unsupported_target();
    signal_empty(); stopped_wait(); toc_control_overlap();
    failed_create_publication(); exhausted_condition_registry(); busy_destroy();
#endif
    std::printf("Startup lwcond host cases=%d failures=%d actual_generated_stub=false\n", cases, failures);
    return failures ? 1 : 0;
}

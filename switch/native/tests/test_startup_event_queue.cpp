/* Host bridge behavior test for guest syscall 128 (sys_event_queue_create) through the REAL dispatcher
 * (lv2_syscall) over real collaborators: coherent guest memory (atomic domain), trace recorder, discovery
 * budget and the REAL owned ps3_lv2_objects registry (pthread guard + wait port). The generated call site is
 * modelled by setting argument registers exactly as the `sc` sequence does; this is a label fixture, not the
 * game. Expected ids/bytes are derived by hand from the lv2 id contract (queue base 0x8D000000, step 0x100,
 * never reused), never captured from the implementation. */
#include <initializer_list>
#include <vector>
#define main inherited_recorder_main
#include "test_startup_recorder.cpp"
#undef main
extern "C" {
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
}

namespace {
int eq_cases, eq_checks, eq_failures;
#define TCHECK(value) do { ++eq_checks; if (!(value)) { ++eq_failures; std::printf("EVENT_QUEUE_FAIL case=%d line=%d expr=%s\n", eq_cases, __LINE__, #value); } } while (0)
constexpr uint32_t OUT = 0x10200, ATTR = 0x10240, SENTINEL = 0x10300, UNMAPPED_PAGE = 0x11000;
constexpr uint32_t QBASE = 0x8D000000u, QSTEP = 0x100u;
constexpr uint64_t CELL_EINVAL = UINT64_C(0xFFFFFFFF80010002), CELL_EAGAIN = UINT64_C(0xFFFFFFFF80010001);

struct EqFixture;
// lv2 guard that records whether the trace reservation was already open when the registry was first entered
// (the create): proves reservation precedes allocation, which an outcome-only check cannot distinguish.
struct ProbeGuard {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    EqFixture *fixture = nullptr;
    int first_lock_batch_active = -1;
    static void lock(void *p);
    static void unlock(void *p) { pthread_mutex_unlock(&static_cast<ProbeGuard *>(p)->mutex); }
};
struct EqFixture {
    Runtime runtime;
    ProbeGuard lv2_guard{};
    ps3_pthread_wait_port port{};
    ps3_lv2_objects lv2{};
    bool service_up = false;
    unsigned seed_writes = 0;
    unsigned operational_calls = 0;
    ps3_clock operational{this, sample_operational};
    static ps3_clock_status sample_operational(void *p, uint64_t *output) {
        ++static_cast<EqFixture *>(p)->operational_calls; *output = 0; return PS3_CLOCK_OK;
    }
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget{policy, operational};
    explicit EqFixture(bool strict_one = false, bool inject = true, bool init_service = true)
        : policy({10, 10, 10, UINT64_C(1000000000)}, 0) {
        TCHECK(runtime.recorder.configure(runtime.events, strict_one ? 1 : 4,
            strict_one ? PpuMemoryTraceRetention::Strict : PpuMemoryTraceRetention::Suffix));
        lv2_guard.fixture = this;
        TCHECK(!ps3_pthread_wait_port_init(&port));
        if (init_service) {
            TCHECK(ps3_lv2_objects_init(&lv2, {&lv2_guard, ProbeGuard::lock, ProbeGuard::unlock}, ps3_pthread_wait_port_port(&port)) == PS3_LV2_OK);
            service_up = true;
        }
        if (inject) runtime.execution.event_queues = &lv2;
        auto &cpu = runtime.execution.ppu.cpu;
        cpu.gpr[11] = 128; cpu.gpr[3] = OUT; cpu.gpr[4] = ATTR; cpu.gpr[5] = 0; cpu.gpr[6] = 127;
        attr(2, 1);
        write_bytes(OUT, {0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44});
        write_bytes(SENTINEL, {1, 2, 3, 4, 5, 6, 7, 8});
        seed_writes = runtime.probe.writes;
    }
    ~EqFixture() {
        if (runtime.execution.ppu.admitted)
            TCHECK(native_startup_release_after_join(runtime.execution) == PS3_GUEST_MEMORY_OK);
        ps3_pthread_wait_port_destroy(&port); pthread_mutex_destroy(&lv2_guard.mutex);
    }
    void write_bytes(uint32_t address, std::initializer_list<unsigned char> bytes) {
        std::vector<unsigned char> v(bytes);
        TCHECK(runtime.binding.coherent_memory.write(runtime.binding.coherent_memory.context, address, v.data(), v.size()) == PS3_GUEST_MEMORY_OK);
    }
    // sys_event_queue_attribute_t: big-endian u32 protocol, s32 type, char name[8] (bytes 1..7 are caller garbage).
    void attr(uint32_t protocol, uint32_t type, unsigned char name_garbage = 0xA5) {
        write_bytes(ATTR, {uint8_t(protocol >> 24), uint8_t(protocol >> 16), uint8_t(protocol >> 8), uint8_t(protocol),
            uint8_t(type >> 24), uint8_t(type >> 16), uint8_t(type >> 8), uint8_t(type),
            uint8_t(name_garbage == 0xA5 ? 0 : name_garbage), name_garbage, name_garbage, name_garbage, name_garbage, name_garbage, name_garbage, name_garbage});
    }
    std::vector<unsigned char> read_bytes(uint32_t address, size_t n) {
        std::vector<unsigned char> v(n);
        TCHECK(runtime.binding.coherent_memory.read(runtime.binding.coherent_memory.context, address, v.data(), n) == PS3_GUEST_MEMORY_OK);
        return v;
    }
    bool admit() {
        return native_startup_admit_budgeted(runtime.execution, runtime.binding, runtime.services, runtime.recorder, budget);
    }
    bool call() {
        terminal_recorder = &runtime.recorder; terminal_guard = &runtime.guard;
        terminal_closed = terminal_unlocked = false;
        active = runtime.control.get();
        bool returned = true;
        try { lv2_syscall(&runtime.execution.ppu.cpu); active = nullptr; }
        catch (const Terminal &) { returned = false; TCHECK(terminal_closed && terminal_unlocked); }
        terminal_recorder = nullptr; terminal_guard = nullptr;
        return returned;
    }
    size_t live() { return service_up ? ps3_lv2_live_count(&lv2, PS3_LV2_KIND_QUEUE) : 0; }
    // Independent derivation: the n-th (0-based) queue ever allocated.
    static uint32_t id_of(uint32_t serial) { return QBASE + QSTEP * serial; }
    void expect_event(size_t index, uint32_t address, uint64_t value) {
        PpuMemoryTraceSample sample{};
        TCHECK(runtime.recorder.read(index, sample));
        TCHECK(sample.sequence == index && sample.event.origin == PpuMemoryTraceOrigin::Hle &&
            sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == address &&
            sample.event.length == 4 && sample.event.value == value && sample.event.status == PS3_GUEST_MEMORY_OK &&
            !sample.event.stored && !sample.event.byte_transfer);
    }
    void expect_no_effects(const ppu_context &before, size_t published = 0) {
        TCHECK(!std::memcmp(&before, &runtime.execution.ppu.cpu, sizeof(before)));
        TCHECK(live() == 0 && runtime.probe.writes == seed_writes);   // only the fixture seed writes
        TCHECK(runtime.recorder.report().published_count == published && !runtime.recorder.report().batch_active);
        TCHECK(read_bytes(OUT, 8) == std::vector<unsigned char>({0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44}));
    }
};

void ProbeGuard::lock(void *p) {
    auto &self = *static_cast<ProbeGuard *>(p);
    pthread_mutex_lock(&self.mutex);
    if (self.first_lock_batch_active < 0) self.first_lock_batch_active = self.fixture->runtime.recorder.report().batch_active;
}
void success(uint32_t protocol, uint32_t type, uint32_t size, unsigned preexisting, uint32_t out_address, unsigned char garbage) {
    ++eq_cases; EqFixture f;
    for (unsigned i = 0; i < preexisting; ++i) {   // queues owned by other native services consumed earlier serials
        uint32_t other = 0; ps3_lv2_queue_attr qa{PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0};
        TCHECK(ps3_lv2_queue_create(&f.lv2, &other, &qa, 0, 8) == PS3_LV2_OK && other == EqFixture::id_of(i));
    }
    f.runtime.execution.ppu.cpu.gpr[3] = out_address;
    f.runtime.execution.ppu.cpu.gpr[6] = size;
    f.attr(protocol, type, garbage);
    TCHECK(f.admit());
    f.lv2_guard.first_lock_batch_active = -1;   // fixture pre-creates above are not the guest call
    auto expected = f.runtime.execution.ppu.cpu; expected.gpr[3] = 0;
    const unsigned writes_before = f.runtime.probe.writes;
    TCHECK(f.call());
    TCHECK(f.lv2_guard.first_lock_batch_active == 1);   // trace reserved before the registry allocated anything
    TCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)));
    const uint32_t id = EqFixture::id_of(preexisting);
    const auto written = f.read_bytes(out_address, 4);
    TCHECK(written == std::vector<unsigned char>({uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)}));
    TCHECK(f.runtime.probe.writes == writes_before + 1);
    ps3_lv2_queue q{};
    TCHECK(ps3_lv2_queue_snapshot(&f.lv2, id, &q) == PS3_LV2_OK);
    TCHECK(q.active && q.id == id && q.protocol == protocol && q.type == type && q.size == size && q.count == 0 && q.receivers == 0 && !q.service_bound);
    TCHECK(f.live() == preexisting + 1);
    TCHECK(f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    f.expect_event(0, out_address, id);
    TCHECK(f.runtime.control->stop == StartupStop::None);
    TCHECK(f.read_bytes(SENTINEL, 8) == std::vector<unsigned char>({1, 2, 3, 4, 5, 6, 7, 8}));
    // Ownership release: the created queue is a real owned object that can be destroyed exactly once.
    // Normal shutdown releases exactly the guest-created queue and leaves other services' queues alone.
    TCHECK(f.runtime.control->event_queue_owned_count == 1 && f.runtime.control->event_queue_owned_ids[0] == id);
    TCHECK(native_startup_release_after_join(f.runtime.execution) == PS3_GUEST_MEMORY_OK);
    TCHECK(f.live() == preexisting && f.runtime.control->event_queue_owned_count == 0);
    TCHECK(ps3_lv2_queue_destroy(&f.lv2, id) == PS3_LV2_ESRCH);
    ps3_lv2_queue gone{};
    TCHECK(ps3_lv2_queue_snapshot(&f.lv2, id, &gone) != PS3_LV2_OK);
}
void shutdown_releases_all_guest_queues() {
    ++eq_cases; EqFixture f; TCHECK(f.admit());
    TCHECK(f.call());
    f.runtime.execution.ppu.cpu.gpr[3] = SENTINEL;
    TCHECK(f.call());
    TCHECK(f.live() == 2 && f.runtime.control->event_queue_owned_count == 2);
    TCHECK(f.runtime.control->event_queue_owned_ids[0] == 0x8D000000u && f.runtime.control->event_queue_owned_ids[1] == 0x8D000100u);
    TCHECK(native_startup_release_after_join(f.runtime.execution) == PS3_GUEST_MEMORY_OK);
    TCHECK(f.live() == 0 && f.runtime.control->event_queue_owned_count == 0 && !f.runtime.execution.ppu.admitted);
}
void shutdown_preserves_ownership_when_service_unavailable() {
    for (int unavailable = 0; unavailable < 3; ++unavailable) {
        ++eq_cases; EqFixture f; TCHECK(f.admit() && f.call());
        StartupControl *control = f.runtime.execution.control;
        if (unavailable == 0) f.runtime.execution.event_queues = nullptr;
        if (unavailable == 1) f.lv2.initialized = false;
        if (unavailable == 2) f.runtime.execution.control = nullptr;
        const auto released = native_startup_release_after_join(f.runtime.execution);
        f.runtime.execution.event_queues = &f.lv2;
        f.lv2.initialized = true;
        f.runtime.execution.control = control;
        TCHECK(released == PS3_GUEST_MEMORY_BUSY);
        TCHECK(f.runtime.execution.ppu.admitted && f.live() == 1);
        TCHECK(control->event_queue_owned_count == 1 && control->event_queue_owned_ids[0] == QBASE);
        TCHECK(native_startup_release_after_join(f.runtime.execution) == PS3_GUEST_MEMORY_OK);
        TCHECK(f.live() == 0 && control->event_queue_owned_count == 0 && !f.runtime.execution.ppu.admitted);
    }
}
void sequential_ids() {
    ++eq_cases; EqFixture f; TCHECK(f.admit());
    TCHECK(f.call());
    f.runtime.execution.ppu.cpu.gpr[3] = SENTINEL;   // second create writes elsewhere
    TCHECK(f.call());
    TCHECK(f.read_bytes(OUT, 4) == std::vector<unsigned char>({0x8D, 0, 0, 0}));
    TCHECK(f.read_bytes(SENTINEL, 4) == std::vector<unsigned char>({0x8D, 0, 0x01, 0}));
    TCHECK(f.live() == 2 && f.runtime.recorder.report().published_count == 2);
    f.expect_event(0, OUT, 0x8D000000u); f.expect_event(1, SENTINEL, 0x8D000100u);
}
void output_overlaps_attr() {   // attr is fully read before the id is stored over it
    ++eq_cases; EqFixture f;
    f.runtime.execution.ppu.cpu.gpr[3] = ATTR + 4;
    TCHECK(f.admit() && f.call());
    ps3_lv2_queue q{};
    TCHECK(ps3_lv2_queue_snapshot(&f.lv2, 0x8D000000u, &q) == PS3_LV2_OK && q.protocol == 2 && q.type == 1);
    TCHECK(f.read_bytes(ATTR + 4, 4) == std::vector<unsigned char>({0x8D, 0, 0, 0}));
}
void reservation(bool same_granule) {
    ++eq_cases; EqFixture f; TCHECK(f.admit());
    auto *cpu = &f.runtime.execution.ppu.cpu;
    const uint32_t reserved = same_granule ? 0x10260 /* zero bytes in the same 128B granule as OUT */ : 0x10400;
    TCHECK(ppu_native_load_reserve32(cpu, reserved) == 0);
    TCHECK(f.call());
    TCHECK(ppu_native_store_conditional32(cpu, reserved, 0x01020304) == !same_granule);
}
// GuestError: a domain-defined CELL return; no output, no trace, no queue, no consumed id.
void guest_error(uint32_t protocol, uint32_t type, uint64_t size_reg, uint64_t cell) {
    ++eq_cases; EqFixture f;
    f.attr(protocol, type);
    f.runtime.execution.ppu.cpu.gpr[6] = size_reg;
    TCHECK(f.admit()); auto expected = f.runtime.execution.ppu.cpu; expected.gpr[3] = cell;
    TCHECK(f.call());
    TCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)));
    TCHECK(f.live() == 0 && f.runtime.recorder.report().published_count == 0 && !f.runtime.recorder.report().batch_active);
    TCHECK(f.read_bytes(OUT, 8) == std::vector<unsigned char>({0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44}));
    uint32_t next = 0; ps3_lv2_queue_attr qa{PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0};
    TCHECK(ps3_lv2_queue_create(&f.lv2, &next, &qa, 0, 1) == PS3_LV2_OK && next == 0x8D000000u);   // serial not consumed
    TCHECK(f.runtime.control->stop == StartupStop::None);
}
void exhausted() {
    ++eq_cases; EqFixture f; ps3_lv2_objects_limit(&f.lv2, PS3_LV2_KIND_QUEUE, 1);
    uint32_t other = 0; ps3_lv2_queue_attr qa{PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0};
    TCHECK(ps3_lv2_queue_create(&f.lv2, &other, &qa, 0, 1) == PS3_LV2_OK);
    TCHECK(f.admit()); auto expected = f.runtime.execution.ppu.cpu; expected.gpr[3] = CELL_EAGAIN;
    TCHECK(f.call());
    TCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)));
    TCHECK(f.live() == 1 && f.runtime.recorder.report().published_count == 0 && !f.runtime.recorder.report().batch_active);
    TCHECK(f.read_bytes(OUT, 4) == std::vector<unsigned char>({0xDE, 0xAD, 0xBE, 0xEF}));
}

// STOP cases: native cannot express the situation as a guest CELL error; nothing may have been effected.
void stop_case(void (*mutate)(EqFixture &), StartupStop expected_stop, bool strict_one = false, bool inject = true, bool init_service = true) {
    ++eq_cases; EqFixture f(strict_one, inject, init_service);
    mutate(f);
    TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    TCHECK(!f.call());
    TCHECK(f.runtime.control->stop == expected_stop);
    f.expect_no_effects(before, strict_one ? 1 : 0);
    if (f.service_up) {   // no serial consumed by the stopped call
        uint32_t next = 0; ps3_lv2_queue_attr qa{PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0};
        TCHECK(ps3_lv2_queue_create(&f.lv2, &next, &qa, 0, 1) == PS3_LV2_OK && next == 0x8D000000u);
    }
}
void decommit(EqFixture &f, uint32_t address) {
    const auto &m = f.runtime.binding.coherent_memory; TCHECK(m.decommit(m.context, address, 4096) == PS3_GUEST_MEMORY_OK);
}
void filled_trace(EqFixture &f) {   // strict capacity 1 exhausted by another published event
    TCHECK(f.runtime.recorder.begin(1));
    f.runtime.recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, SENTINEL, 8, 1));
    f.runtime.recorder.finish();
}

void write_failure_rolls_back() {
    ++eq_cases; EqFixture f; f.runtime.execution.ppu.cpu.gpr[3] = 0x10128;   // MemoryProbe fail_toc faults this address
    f.runtime.probe.fail_toc = true;
    TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    TCHECK(!f.call());
    TCHECK(f.runtime.control->stop == StartupStop::ProviderFailure);
    TCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
    TCHECK(f.live() == 0);                                   // rolled back: no leaked queue
    TCHECK(f.runtime.recorder.report().published_count == 0 && !f.runtime.recorder.report().batch_active);
    uint32_t next = 0; ps3_lv2_queue_attr qa{PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0};
    TCHECK(ps3_lv2_queue_create(&f.lv2, &next, &qa, 0, 1) == PS3_LV2_OK && next == 0x8D000100u);   // ids are never reused
}
// Rollback itself refused by the real domain (a receiver registered during the failing store): ownership is
// retained, recorded, and the stop is terminal; nothing is silently leaked.
struct ReceiverInjector {
    EqFixture *fixture; ps3_guest_memory real;
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *b, size_t n) {
        auto &self = *static_cast<ReceiverInjector *>(p);
        if (a == 0x10128) { self.fixture->lv2.queues[0].receivers = 1; return PS3_GUEST_MEMORY_PLATFORM_FAILURE; }
        return self.real.write(self.real.context, a, b, n);
    }
    static ps3_guest_memory_status read(void *p, uint32_t a, void *b, size_t n) { auto &s = *static_cast<ReceiverInjector *>(p); return s.real.read(s.real.context, a, b, n); }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) { auto &s = *static_cast<ReceiverInjector *>(p); return s.real.commit(s.real.context, a, n); }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) { auto &s = *static_cast<ReceiverInjector *>(p); return s.real.decommit(s.real.context, a, n); }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n) { auto &s = *static_cast<ReceiverInjector *>(p); return s.real.zero(s.real.context, a, n); }
};
void rollback_refused_retains_ownership() {
    ++eq_cases; EqFixture f; ReceiverInjector injector{&f, f.runtime.probe.backing};
    f.runtime.probe.backing = {&injector, ReceiverInjector::commit, ReceiverInjector::decommit, ReceiverInjector::read, ReceiverInjector::write, ReceiverInjector::zero};
    f.runtime.execution.ppu.cpu.gpr[3] = 0x10128;
    TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    TCHECK(!f.call());
    TCHECK(f.runtime.control->stop == StartupStop::ProviderFailure);
    TCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
    TCHECK(f.live() == 1 && f.runtime.control->event_queue_retained_id == 0x8D000000u);
    TCHECK(f.runtime.recorder.report().published_count == 0 && !f.runtime.recorder.report().batch_active);
    TCHECK(f.runtime.control->event_queue_owned_count == 1 && f.runtime.control->event_queue_owned_ids[0] == 0x8D000000u);
    // Shutdown with a receiver still registered must RETAIN the record and report BUSY, not drop it.
    TCHECK(native_startup_release_after_join(f.runtime.execution) == PS3_GUEST_MEMORY_BUSY);
    TCHECK(f.live() == 1 && f.runtime.control->event_queue_owned_count == 1 && f.runtime.execution.ppu.admitted);
    f.lv2.queues[0].receivers = 0;                                            // receiver gone: owner can now release
    TCHECK(native_startup_release_after_join(f.runtime.execution) == PS3_GUEST_MEMORY_OK);
    TCHECK(f.live() == 0 && f.runtime.control->event_queue_owned_count == 0);
    f.runtime.probe.backing = injector.real;
}
void cancel_after_commit() {   // MemoryProbe cancels when OUT is stored: the commit is real and stays owned
    ++eq_cases; EqFixture f; f.runtime.probe.cancel_output = true;
    TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    TCHECK(!f.call());
    TCHECK(f.runtime.control->stop == StartupStop::Cancelled);
    TCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));   // r3 suppressed
    TCHECK(f.live() == 1 && f.read_bytes(OUT, 4) == std::vector<unsigned char>({0x8D, 0, 0, 0}));
    TCHECK(f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    f.expect_event(0, OUT, 0x8D000000u);
}
void unsupported_neighbors() {
    // Receive and port calls have dedicated behavior suites; queue destruction and noncanonical numbers stay fenced.
    for (uint64_t number : {UINT64_C(129), UINT64_C(0x100000080)}) {
        ++eq_cases; EqFixture f; f.runtime.execution.ppu.cpu.gpr[11] = number;
        // The legacy memory-family gate requires these capabilities before rejecting any other number.
        f.runtime.services.allocate_memory = [](void *, uint64_t, uint64_t, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        f.runtime.services.free_memory = [](void *, uint32_t) -> ps3_user_memory_service_result { std::abort(); };
        f.runtime.services.query_memory = [](void *, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
        TCHECK(!f.call() && f.runtime.control->stop == StartupStop::UnsupportedSyscall);
        f.expect_no_effects(before);
    }
}
}

int main() {
    shutdown_preserves_ownership_when_service_unavailable();
    success(2, 1, 127, 0, OUT, 0xA5);            // the observed guest call: PRIORITY, PPU, size 127
    success(1, 2, 1, 0, OUT, 0x00);              // FIFO, SPU, size 1
    success(2, 1, 127, 2, OUT, 0xFF);            // id follows queues already owned by other services
    success(1, 1, 64, 0, OUT + 3, 0x5A);         // unaligned output: stored byte-exact
    success(2, 1, 20, 0, OUT, 0xFF);             // name bytes (incl. byte 0) are indeterminate: no NUL required
    shutdown_releases_all_guest_queues();
    sequential_ids(); output_overlaps_attr(); reservation(true); reservation(false);
    guest_error(2, 1, 0, CELL_EINVAL);
    guest_error(2, 1, 128, CELL_EINVAL);
    guest_error(2, 1, 0xFFFFFFFFu, CELL_EINVAL);           // s32 -1
    guest_error(2, 1, 0x80000000u, CELL_EINVAL);           // s32 minimum
    guest_error(0, 1, 127, CELL_EINVAL); guest_error(3, 1, 127, CELL_EINVAL); guest_error(4, 1, 127, CELL_EINVAL);
    guest_error(2, 0, 127, CELL_EINVAL); guest_error(2, 3, 127, CELL_EINVAL); guest_error(2, 0x80000001u, 127, CELL_EINVAL);
    exhausted();
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[5] = 1; }, StartupStop::UnsupportedSyscall);       // IPC key
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[5] = UINT64_C(0x8000000000000000); }, StartupStop::UnsupportedSyscall);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] = 0; }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[4] = 0; }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] |= UINT64_C(0x100000000); }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[4] |= UINT64_C(0x100000000); }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] = UINT64_C(0xFFFFFFFFFFFF0000); }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] = UINT32_MAX - 1; }, StartupStop::InvalidMemory);   // output span overflows
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[4] = UINT32_MAX - 7; }, StartupStop::InvalidMemory);   // 16-byte attr overflows
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] = 0x11FFE; }, StartupStop::InvalidMemory);          // output crosses RAM end
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[4] = 0x11FF8; }, StartupStop::InvalidMemory);          // attr crosses RAM end
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] = UNMAPPED_PAGE; decommit(f, UNMAPPED_PAGE); }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[4] = UNMAPPED_PAGE; decommit(f, UNMAPPED_PAGE); }, StartupStop::InvalidMemory);
    stop_case([](EqFixture &f) { f.runtime.ram.writable = false; }, StartupStop::InvalidMemory);                        // read-only output
    stop_case([](EqFixture &f) { f.runtime.execution.ppu.cpu.gpr[3] = 0x20000; }, StartupStop::InvalidMemory);          // outside RAM windows
    stop_case([](EqFixture &) {}, StartupStop::MissingServices, false, false, true);    // service not injected
    stop_case([](EqFixture &) {}, StartupStop::MissingServices, false, true, false);    // injected but never initialised
    stop_case([](EqFixture &f) { f.runtime.control->cancelled.store(true); }, StartupStop::Cancelled);
    stop_case(filled_trace, StartupStop::TraceCapacity, true);
    write_failure_rolls_back(); rollback_refused_retains_ownership(); cancel_after_commit();
    unsupported_neighbors();
    std::printf("Event queue create host cases=%d checks=%d failures=%d inherited_failures=%d native_sdk_proof=false\n", eq_cases, eq_checks, eq_failures, failures);
    return eq_failures || failures ? 1 : 0;
}

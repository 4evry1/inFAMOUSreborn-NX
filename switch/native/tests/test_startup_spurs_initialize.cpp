/* Host bridge behavior test for aa6269a8 (cellSpursInitializeWithAttribute) through the REAL dispatcher
 * (ps3_hle_call / lv2_syscall) and the REAL composition (startup_spurs_compose) over real collaborators:
 * atomic domain + coherent guest memory, shared user-memory quota, lv2 objects, SPU group policy (configured
 * only through the guest's own syscall 169), PPU service registry with real host threads, the kernel-1 idle
 * service, and the real lwmutex/lwcond registries. The generated entry is modelled by the call fixture (it
 * sets the PPU argument registers exactly as the generated import stub does); this is a label fixture, not the
 * game. Expected values are derived by hand from the contract (lv2 id bases, W-table), never captured. */
#include "../adapters/startup_execution.cpp"
#include "../adapters/startup_spurs_sync_report.h"
#include "../adapters/startup_capacity_policy.h"
#include "../adapters/startup_spurs_composition.cpp"
extern "C" {
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
}
#include <cerrno>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include <pthread.h>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("SPURS_INITIALIZE_BRIDGE_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
// Test-only, per-thread record of the tracked guards (domain/policy/lv2/ppu) this thread currently holds. Real pthread
// locks are taken first and recorded only after they succeed; unlock is non-LIFO. Overflow/unmatched unlock fail loudly.
constexpr size_t HELD_GUARD_CAPACITY = 8;
struct HeldGuards { const void *guard[HELD_GUARD_CAPACITY]; size_t count; };
thread_local HeldGuards held_guards;
// Test-only per-thread acquisition log: which tracked guard was locked and how many tracked guards this thread already held.
// A lock of a guard the thread already holds is a nested same-guard acquisition: it is counted and NOT forwarded to the real
// (nonrecursive) mutex, so a violation fails loudly instead of deadlocking.
constexpr size_t GUARD_LOG_CAPACITY = 64;
struct GuardEvent { const void *guard; size_t depth_before; };
thread_local GuardEvent guard_log[GUARD_LOG_CAPACITY];
thread_local size_t guard_log_count, nested_same_guard_acquisitions;
void guard_log_reset() { guard_log_count = 0; nested_same_guard_acquisitions = 0; }
void tracked_lock(void *context) {
    bool nested = false;
    for (size_t i = 0; i < held_guards.count; ++i) nested |= held_guards.guard[i] == context;
    if (guard_log_count < GUARD_LOG_CAPACITY) guard_log[guard_log_count++] = {context, held_guards.count};
    if (nested) ++nested_same_guard_acquisitions;
    else ps3_pthread_guard_port(static_cast<ps3_pthread_guard *>(context)).lock(context);
    CHECK(held_guards.count < HELD_GUARD_CAPACITY);
    if (held_guards.count < HELD_GUARD_CAPACITY) held_guards.guard[held_guards.count++] = context;
}
void tracked_unlock(void *context) {
    size_t i = 0;
    while (i < held_guards.count && held_guards.guard[i] != context) ++i;
    CHECK(i < held_guards.count);
    bool still_held = false;
    if (i < held_guards.count) {
        for (; i + 1 < held_guards.count; ++i) held_guards.guard[i] = held_guards.guard[i + 1];
        --held_guards.count;
        for (size_t k = 0; k < held_guards.count; ++k) still_held |= held_guards.guard[k] == context;
    }
    if (!still_held) ps3_pthread_guard_port(static_cast<ps3_pthread_guard *>(context)).unlock(context);
}
ps3_instance_guard tracked_guard_port(ps3_pthread_guard *guard) { return {guard, tracked_lock, tracked_unlock}; }
bool thread_holds_no_tracked_guard() { return held_guards.count == 0; }
constexpr uint32_t INSTANCE1 = 0x7CF100, INSTANCE2 = 0x7D0380, ATTR = 0xD00FF990, STACK = 0xD00FF910, TOC_SLOT = STACK + 0x28;
constexpr uint32_t STACK_BASE = 0xD00FF000, STACK_SPAN = 0x3000, HOST_STACK = 0x800000, HOST_SIZE = 0x900000;
constexpr uint64_t TOC = UINT64_C(0x123456789abcdef0);
constexpr uint32_t WINDOW = 0x400000, WINDOW_END = 0x700000, QUOTA = 0x300000;
// Target-profile fixture storage: sized generously, registries are initialised with the shared startup capacity policy values
// (mutex objects / atomic owners from startup_capacity_policy.h; waiters, conditions and bindings fixed at the design's 32).
constexpr size_t TARGET_STORAGE_MUTEXES = 128, TARGET_SYNC_TABLE = 32;
// Two guest windows over one host space: identity below 8 MiB, and the stack/attribute page window.
struct GuestSpace {
    ElfTestHostSpace *space = elf_test_host_space_create(HOST_SIZE);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    GuestSpace() { CHECK(space && backing.commit(backing.context, 0, HOST_SIZE) == PS3_GUEST_MEMORY_OK); }
    ~GuestSpace() { elf_test_host_space_destroy(space); }
    static bool map(uint32_t a, uint64_t n, uint32_t &host) {
        if (uint64_t(a) + n <= HOST_STACK) { host = a; return true; }
        if (a >= STACK_BASE && uint64_t(a) - STACK_BASE + n <= STACK_SPAN) { host = HOST_STACK + (a - STACK_BASE); return true; }
        return false;
    }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) {
        uint32_t h; return map(a, n, h) ? static_cast<GuestSpace *>(p)->backing.commit(static_cast<GuestSpace *>(p)->backing.context, h, n) : PS3_GUEST_MEMORY_INVALID_RANGE; }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) {
        uint32_t h; return map(a, n, h) ? static_cast<GuestSpace *>(p)->backing.decommit(static_cast<GuestSpace *>(p)->backing.context, h, n) : PS3_GUEST_MEMORY_INVALID_RANGE; }
    static ps3_guest_memory_status read(void *p, uint32_t a, void *b, size_t n) {
        uint32_t h; return map(a, n, h) ? static_cast<GuestSpace *>(p)->backing.read(static_cast<GuestSpace *>(p)->backing.context, h, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE; }
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *b, size_t n) {
        uint32_t h; return map(a, n, h) ? static_cast<GuestSpace *>(p)->backing.write(static_cast<GuestSpace *>(p)->backing.context, h, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE; }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n) {
        uint32_t h; return map(a, n, h) ? static_cast<GuestSpace *>(p)->backing.zero(static_cast<GuestSpace *>(p)->backing.context, h, n) : PS3_GUEST_MEMORY_INVALID_RANGE; }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};
struct ConditionWait {   // lwmutex/lwcond wait port (pthread condvar)
    pthread_cond_t condition{};
    ConditionWait() {
        pthread_condattr_t attr;
        CHECK(!pthread_condattr_init(&attr) && !pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) &&
              !pthread_cond_init(&condition, &attr) && !pthread_condattr_destroy(&attr));
    }
    ~ConditionWait() { CHECK(!pthread_cond_destroy(&condition)); }
    static ps3_wait_status now(void *, uint64_t *output) {
        timespec value{}; if (clock_gettime(CLOCK_MONOTONIC, &value)) std::abort();
        *output = uint64_t(value.tv_sec) * UINT64_C(1000000000) + uint64_t(value.tv_nsec); return PS3_WAIT_READY;
    }
    static ps3_wait_status park(void *context, const ps3_instance_guard *guard, uint64_t deadline, ps3_wait_cancel cancel) {
        if (cancel.is_cancelled && cancel.is_cancelled(cancel.context)) return PS3_WAIT_CANCELLED;
        uint64_t value = 0; now(context, &value);
        const uint64_t tick = deadline < value + 2000000 ? deadline : value + 2000000;
        const timespec until{time_t(tick / 1000000000), long(tick % 1000000000)};
        const int status = pthread_cond_timedwait(&static_cast<ConditionWait *>(context)->condition, static_cast<pthread_mutex_t *>(guard->context), &until);
        if (status && status != ETIMEDOUT) std::abort();
        return PS3_WAIT_READY;
    }
    static void notify(void *context) { if (pthread_cond_broadcast(&static_cast<ConditionWait *>(context)->condition)) std::abort(); }
    ps3_wait_port port() { return {this, now, park, notify}; }
};
ps3_lwcond_thread_status main_exists(void *context, uint32_t id, unsigned *output) {
    *output = id == *static_cast<uint32_t *>(context); return PS3_LWCOND_THREAD_OK;
}
struct Runtime {
    GuestSpace guest;
    ps3_pthread_guard g_dom, g_mem, g_lv2, g_ppu, g_pol, g_mutex, g_cond;
    ps3_pthread_wait_port port;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[16]{};   // small fixtures use all 16; the target profile initialises STARTUP_ATOMIC_OWNER_CAPACITY of them
    ps3_atomic_pin_record pins[128]{};
    ps3_user_memory memory{};
    ps3_user_memory_slot slots[4]{};
    ps3_spu_resource_policy policy{};
    ConditionWait mutex_wait, cond_wait;
    ps3_lwmutex_registry mutexes{};
    ps3_lwmutex_object mutex_objects[TARGET_STORAGE_MUTEXES]{};
    ps3_lwmutex_waiter mutex_waiters[TARGET_SYNC_TABLE]{};
    ps3_lwcond_registry conditions{};
    ps3_lwcond_object condition_objects[TARGET_SYNC_TABLE]{};
    ps3_lwcond_waiter condition_waiters[TARGET_SYNC_TABLE]{};
    uint32_t main_id;
    PpuMemoryRamRange ranges[2]{{0x7CF000, 0x3000, true}, {STACK_BASE, STACK_SPAN, true}};
    std::unique_ptr<PpuMemoryBinding> binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(16384);
    PpuMemoryTraceRecorder recorder;
    static ps3_clock_status sample(void *, uint64_t *value) { *value = 1; return PS3_CLOCK_OK; }
    ps3_clock clock{nullptr, sample};
    infamous::discovery::DiscoveryBudget budget_policy{{64, 64, 64, UINT64_C(1000000000)}, 0};
    DiscoveryBudgetBinding budget{budget_policy, clock};
    // composition storage (zero-filled, stable)
    std::unique_ptr<StartupSpursComposition> comp{new StartupSpursComposition};
    ps3_pthread_service_thread threads[PS3_SPURS_MAX_INSTANCES][3]{};
    std::vector<unsigned char> local_stores = std::vector<unsigned char>(size_t(PS3_SPURS_LS_SLOTS) * PS3_SPURS_K1_LS_BYTES);
    StartupSpursPorts ports;
    bool composed = false;
    StartupSpursComposeStatus compose_status = StartupSpursComposeStatus::InvalidArgument;

    // window/quota/slot_count default to the original 3 MiB fixture; the prior-allocation fixture widens all three.
    // target_profile=true: explicit startup-capacity-policy fixture (mutex objects/atomic owners from the policy header, 32 waiters,
    // 32 conditions, 32 bindings); false keeps the original small-cap failure fixtures (4 objects, 4 conditions, 16 owners).
    explicit Runtime(uint32_t thread_id = 1, bool compose = true, bool admit = true, uint32_t window_begin = WINDOW,
                     uint32_t window_end = WINDOW_END, uint64_t quota = QUOTA, size_t slot_count = 2,
                     size_t binding_capacity = 4, bool target_profile = false) : main_id(thread_id) {
        const size_t mutex_count = target_profile ? size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY) : 4;
        const size_t waiter_count = target_profile ? TARGET_SYNC_TABLE : 8, condition_count = target_profile ? TARGET_SYNC_TABLE : 4;
        const size_t condition_waiter_count = target_profile ? TARGET_SYNC_TABLE : 4, binding_count = target_profile ? TARGET_SYNC_TABLE : binding_capacity;
        const size_t owner_count = target_profile ? size_t(STARTUP_ATOMIC_OWNER_CAPACITY) : 16;
        CHECK(mutex_count <= TARGET_STORAGE_MUTEXES && owner_count <= 16);
        CHECK(!ps3_pthread_guard_init(&g_dom) && !ps3_pthread_guard_init(&g_mem) && !ps3_pthread_guard_init(&g_lv2) &&
              !ps3_pthread_guard_init(&g_ppu) && !ps3_pthread_guard_init(&g_pol) && !ps3_pthread_guard_init(&g_mutex) &&
              !ps3_pthread_guard_init(&g_cond) && !ps3_pthread_wait_port_init(&port));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), tracked_guard_port(&g_dom), owners, owner_count, pins, 128) == PS3_GUEST_MEMORY_OK);
        const ps3_guest_memory coherent = ps3_atomic_domain_memory(&domain);
        const ps3_user_memory_config mc{window_begin, window_end, quota, coherent, ps3_pthread_guard_port(&g_mem)};
        CHECK(slot_count <= 4 && ps3_user_memory_init(&memory, &mc, slots, slot_count) == PS3_USER_MEMORY_OK);
        CHECK(ps3_spu_resource_policy_init(&policy, tracked_guard_port(&g_pol)) == PS3_SPU_POLICY_OK);
        CHECK(ps3_lwmutex_init_with_conditions(&mutexes, &domain, tracked_guard_port(&g_mutex), mutex_wait.port(),
            mutex_objects, mutex_count, mutex_waiters, waiter_count, binding_count).status == PS3_LWMUTEX_OK);
        CHECK(ps3_lwcond_init(&conditions, &mutexes, tracked_guard_port(&g_cond), cond_wait.port(), {&main_id, main_exists},
            condition_objects, condition_count, condition_waiters, condition_waiter_count).status == PS3_LWCOND_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges, 2));
        CHECK(ps3_atomic_commit(&domain, 0x7CF000, 0x3000) == PS3_GUEST_MEMORY_OK && ps3_atomic_commit(&domain, STACK_BASE, STACK_SPAN) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding->coherent_memory;
        execution.control = control.get();
        execution.conditions = &conditions;
        execution.spu_resources = &policy;
        execution.guest_priority = 250; execution.has_guest_priority = true;
        execution.ppu.cpu.thread_id = thread_id;
        execution.ppu.cpu.gpr[1] = STACK; execution.ppu.cpu.gpr[2] = TOC; execution.ppu.cpu.lr = 0x2cfe70;
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Strict, 0));
        if (admit) CHECK(native_startup_admit_budgeted(execution, *binding, services, recorder, budget));
        for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) ports.local_stores[i] = &local_stores[size_t(i) * PS3_SPURS_K1_LS_BYTES];
        ports.notify = ps3_pthread_wait_port_port(&port);
        ports.lv2_guard = tracked_guard_port(&g_lv2); ports.ppu_guard = tracked_guard_port(&g_ppu);
        for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
            for (unsigned r = 0; r < 3; ++r) ports.threads[i][r] = ps3_pthread_service_thread_port(&threads[i][r]);
        if (compose) {
            compose_status = startup_spurs_compose(execution, *comp, domain, memory, policy, ports);
            composed = compose_status == StartupSpursComposeStatus::Ok;
            CHECK(composed && execution.spurs == &comp->runtime);
        }
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        binding.reset();
        if (composed) CHECK(ps3_user_memory_cleanup(&memory).status == PS3_USER_MEMORY_OK);
        ps3_lwcond_report cleanup_report{};
        CHECK(ps3_lwcond_cleanup(&conditions, &cleanup_report).status == PS3_LWCOND_OK);
        CHECK(ps3_lwmutex_cleanup(&mutexes).status == PS3_LWMUTEX_OK);
        ps3_pthread_wait_port_destroy(&port);
        ps3_pthread_guard_destroy(&g_dom); ps3_pthread_guard_destroy(&g_mem); ps3_pthread_guard_destroy(&g_lv2);
        ps3_pthread_guard_destroy(&g_ppu); ps3_pthread_guard_destroy(&g_pol); ps3_pthread_guard_destroy(&g_mutex); ps3_pthread_guard_destroy(&g_cond);
    }
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n) { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    void put32(uint32_t a, uint32_t v) { const unsigned char e[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; put(a, e, 4); }
    uint32_t get32(uint32_t a) { unsigned char b[4]{}; CHECK(mem().read(mem().context, a, b, 4) == PS3_GUEST_MEMORY_OK); return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
    uint8_t get8(uint32_t a) { uint8_t b = 0; CHECK(mem().read(mem().context, a, &b, 1) == PS3_GUEST_MEMORY_OK); return b; }
    uint64_t used() { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&memory, &i) == PS3_USER_MEMORY_OK); return i.used_bytes; }
    // CellSpursAttribute (v1 wire image): revision 2, sdk 0x250001, nSpus, spuPriority, ppuPriority, exit 0, flags 0.
    void attribute(uint32_t nspus, uint32_t spu_prio, uint32_t ppu_prio, uint32_t flags = 0) {
        unsigned char image[512]{};
        const uint32_t w[5]{2, 0x250001, nspus, spu_prio, ppu_prio};
        for (unsigned i = 0; i < 5; ++i) for (unsigned b = 0; b < 4; ++b) image[4 * i + b] = uint8_t(w[i] >> (24 - 8 * b));
        for (unsigned b = 0; b < 4; ++b) image[0x28 + b] = uint8_t(flags >> (24 - 8 * b));
        put(ATTR, image, sizeof image);
    }
    void guarded(void (*body)(Runtime &), Runtime &r) { active = control.get(); control->cpu = &execution.ppu.cpu; control->stop = StartupStop::None;
        try { body(r); active = nullptr; } catch (const Terminal &) {} }
    // sys_spu_initialize through the guest's own syscall-169 dispatch (the only policy configuration path).
    void spu_initialize(uint64_t usable, uint64_t raw) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[3] = usable; cpu.gpr[4] = raw; cpu.gpr[11] = 169;
        active = control.get(); control->cpu = &cpu; control->stop = StartupStop::None;
        try { lv2_syscall(&cpu); active = nullptr; } catch (const Terminal &) {}
        CHECK(control->stop == StartupStop::None && cpu.gpr[3] == 0);
    }
    // The generated import stub: r3=instance, r4=attribute, r5..r8 as the real caller passes them.
    void call_initialize(uint32_t instance) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[3] = instance; cpu.gpr[4] = ATTR; cpu.gpr[5] = cpu.gpr[6] = cpu.gpr[7] = cpu.gpr[8] = 0;
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        control->stop = StartupStop::None;
        active = control.get(); control->cpu = &cpu;
        try { ps3_hle_call(0xaa6269a8, &cpu); active = nullptr; } catch (const Terminal &) {}
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) {
    CHECK(thread_holds_no_tracked_guard());   // the exiting thread owns none of the tracked guards (another thread may)
    throw Terminal{};
}
namespace {
constexpr uint64_t K1_IDLE = PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS;
constexpr uint32_t W22_LWMUTEX = 0xDB0, W23_LWCOND = 0xDC8, K1_SPAN = 0x1000;
// M3 trace-coverage helpers. The PPU trace journal has exactly one producer (the PPU thread); the kernel-1
// worker is an SPU agent whose store-conditionals on the two instance lines are NOT PPU effects and never
// enter it (ppu_memory_trace.h: "One CPU owner is the sole producer"). Those bytes are exempted by name:
// spuIdling (+0x73), sysSrvOnSpu (+0xC8), traceInitialised (+0xCC), published by spurs_kernel1_service.c.
std::vector<unsigned char> snapshot(Runtime &r, uint32_t address, size_t length) {
    std::vector<unsigned char> bytes(length);
    CHECK(r.mem().read(r.mem().context, address, bytes.data(), length) == PS3_GUEST_MEMORY_OK);
    return bytes;
}
// Replays every retained HLE write event onto a shadow copy of a pre-call image.
void replay_hle_writes(Runtime &r, uint32_t base, std::vector<unsigned char> &shadow) {
    PpuMemoryTraceSample sample;
    for (size_t i = 0; r.recorder.read(i, sample); ++i) {
        const auto &e = sample.event;
        if (e.origin != PpuMemoryTraceOrigin::Hle || e.operation != PpuMemoryTraceOperation::Write) continue;
        for (size_t b = 0; b < e.length; ++b) {
            const uint64_t a = uint64_t(e.address) + b;
            if (a < base || a - base >= shadow.size()) continue;
            shadow[a - base] = e.byte_transfer ? e.bytes[b] : uint8_t(e.value >> (8 * (e.length - 1 - b)));
        }
    }
}
// Number of guest bytes whose final value is not explained by the traced PPU writes (K1 SPU-agent bytes exempt).
size_t untraced_bytes(Runtime &r, uint32_t base, const std::vector<unsigned char> &before, bool k1_started) {
    auto shadow = before;
    replay_hle_writes(r, base, shadow);
    const auto actual = snapshot(r, base, before.size());
    size_t bad = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (k1_started && (i == 0x73 || i == 0xC8 || i == 0xCC)) continue;
        if (shadow[i] == actual[i]) continue;
        std::printf("SPURS_INITIALIZE_BRIDGE_UNTRACED offset=0x%zx traced=0x%02x actual=0x%02x\n", i, shadow[i], actual[i]);
        ++bad;
    }
    return bad;
}
long find_hle_write(Runtime &r, uint32_t address, size_t nth = 0) {
    PpuMemoryTraceSample sample;
    for (size_t i = 0; r.recorder.read(i, sample); ++i)
        if (sample.event.origin == PpuMemoryTraceOrigin::Hle && sample.event.operation == PpuMemoryTraceOperation::Write &&
            sample.event.address == address && nth-- == 0) return long(i);
    return -1;
}
size_t live_mutexes(Runtime &r) { size_t n = 0; for (auto &o : r.mutex_objects) n += o.state != 0; return n; }
size_t live_conditions(Runtime &r) { size_t n = 0; for (auto &o : r.condition_objects) n += o.state != 0; return n; }
void run_one(uint32_t main_thread_id, uint32_t expected_ppu1, uint32_t expected_ppu0) {
    Runtime r(main_thread_id);
    CHECK(r.execution.spurs == &r.comp->runtime && r.execution.spurs->config.creator_priority == 250);
    CHECK(r.execution.event_queues == &r.comp->lv2);   // guest sc128 injects the registry SPURS owns
    CHECK(ps3_ppu_threads_main_id(&r.comp->ppu) == main_thread_id);
    // Group policy only via the real syscall 169 (usable 6 raw 0, as the real game calls it).
    r.spu_initialize(6, 0);
    CHECK(ps3_spu_resource_policy_snapshot(&r.policy).current.managed_limit == 6);
    r.attribute(5, 128, 249);
    unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8);
    const auto before = snapshot(r, INSTANCE1, K1_SPAN);
    r.call_initialize(INSTANCE1);
    auto &c = *r.control;
    CHECK(c.stop == StartupStop::None);
    // M3: the real lwmutex (W22, 24 B) and lwcond (W23, 8 B) registry writes are successful PPU guest effects:
    // each byte that differs from the pre-call image must be explained by a published HLE trace event.
    CHECK(untraced_bytes(r, INSTANCE1, before, true) == 0);
    CHECK(find_hle_write(r, INSTANCE1 + W22_LWMUTEX, 1) >= 0 && find_hle_write(r, INSTANCE1 + W23_LWCOND, 1) >= 0);
    CHECK(c.spurs_initialize_calls == 1 && c.spurs_initialize_returns == 1 && c.spurs_initialize_has_result);
    CHECK(c.spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_OK && c.spurs_initialize_result.rc == 0 && c.spurs_initialize_result.step == 16);
    CHECK(c.spurs_initialize_arguments[0] == INSTANCE1 && c.spurs_initialize_arguments[1] == ATTR);
    CHECK(r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[2] == TOC && r.execution.ppu.cpu.gpr[1] == STACK);
    // 62 sink writes (14+20+6+1+5+5+2+1+1+1+1+1+1+4, counted by hand from the W-table) + the TOC write
    CHECK(c.spurs_initialize_writes == 63);
    unsigned char toc_bytes[8]; r.mem().read(r.mem().context, TOC_SLOT, toc_bytes, 8);
    const unsigned char want_toc[8]{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    CHECK(!std::memcmp(toc_bytes, want_toc, 8));
    // independent contract values: lv2 id bases, group base, service ids, W-table fields
    CHECK(r.get8(INSTANCE1 + 0x76) == 5 && r.get32(INSTANCE1 + 0xD88) == 249 && r.get32(INSTANCE1 + 0xD84) == 128);
    CHECK(r.get32(INSTANCE1 + 0xDA0) == 2 && r.get32(INSTANCE1 + 0xDA4) == 0x250001);
    CHECK(r.get32(INSTANCE1 + 0xD30) == 0x04000100u && r.get32(INSTANCE1 + 0xD5C) == 0x8D000000u && r.get32(INSTANCE1 + 0xD60) == 0x0E000000u);
    CHECK(r.get8(INSTANCE1 + 0xC9) == 16);
    CHECK(r.get32(INSTANCE1 + 0xD24) == expected_ppu0 && r.get32(INSTANCE1 + 0xD2C) == expected_ppu1   /* ppu0=handler (2nd service), ppu1=helper (1st) */ && r.get32(INSTANCE1 + 0xD68) == 1);
    CHECK(expected_ppu1 != main_thread_id && expected_ppu0 != main_thread_id);   // L5 at the bridge
    CHECK(r.used() == 0x14C000);                                                   // 5 x 256 KiB + 0x8000 + 0x4000
    // R7 ledger, empty-quota baseline: nothing existed before, so total == delta == the SPURS charge.
    CHECK(c.spurs_quota.before.status == PS3_USER_MEMORY_OK && c.spurs_quota.after.status == PS3_USER_MEMORY_OK);
    CHECK(c.spurs_quota.prior_used == 0 && c.spurs_quota.delta_charge == 0x14C000 && c.spurs_quota.total_used == 0x14C000);
    CHECK(startup_spurs_quota_charge_is(c.spurs_quota, STARTUP_SPURS_FIRST_INSTANCE_CHARGE));
    auto &in = r.execution.spurs->instances[0];
    CHECK(in.state == PS3_SPURS_INSTANCE_ACTIVE && in.nspus == 5 && in.kernel_initialized);
    CHECK(ps3_spurs_k1_phase_of(&in.kernel) == PS3_SPURS_K1_PHASE_QUIESCENT);
    for (uint32_t n = 0; n < 5; ++n) {
        CHECK(ps3_spurs_k1_context_state_of(&in.kernel, n) == PS3_SPURS_K1_CTX_BLOCKED_LR);
        CHECK(ps3_spurs_k1_counter(&in.kernel, n, K1_IDLE) >= 1);
    }
    for (uint32_t n = 5; n < 8; ++n) CHECK(ps3_spurs_k1_context_state_of(&in.kernel, n) == PS3_SPURS_K1_CTX_NOT_STARTED);
    ps3_spu_group_record group{};
    CHECK(ps3_spu_group_snapshot(&r.policy, 0x04000100u, &group) == PS3_LV2_OK && group.state == PS3_SPU_GROUP_RUNNING && group.executor == &in.kernel && group.num == 5);
    ps3_spurs_stop_info fence{};
    CHECK(ps3_spurs_runtime_poll_stop(&r.comp->runtime, &fence) == 0);
    // trace and guest memory agree: every published HLE write is exactly what the guest bytes hold
    const auto report = r.recorder.report();
    CHECK(report.published_count >= 63 && !report.batch_active && !report.remaining_publications);
    // second instance (nSpus=1), supported by the transaction; quota accumulates
    r.attribute(1, 128, 249);
    r.call_initialize(INSTANCE2);
    CHECK(c.stop == StartupStop::None && c.spurs_initialize_calls == 2 && c.spurs_initialize_returns == 2 && r.execution.ppu.cpu.gpr[3] == 0);
    CHECK(r.used() == 0x198000 && r.get8(INSTANCE2 + 0x76) == 1 && r.get32(INSTANCE2 + 0xD30) == 0x04000200u);
    // the ledger is per call: the second instance's delta excludes the first instance, which is now "prior"
    CHECK(c.spurs_quota.measured && c.spurs_quota.prior_used == 0x14C000 && c.spurs_quota.delta_charge == 0x4C000 && c.spurs_quota.total_used == 0x198000);
    CHECK(!startup_spurs_quota_charge_is(c.spurs_quota, STARTUP_SPURS_FIRST_INSTANCE_CHARGE));
    // shutdown BEFORE anything is released: joins every service, returns every charge
    CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_OK);
    CHECK(r.used() == 0 && ps3_lv2_live_count(&r.comp->lv2, 0) == 0 && ps3_lv2_live_count(&r.comp->lv2, 1) == 0 && ps3_lv2_live_count(&r.comp->lv2, 2) == 0);
    CHECK(ps3_ppu_threads_live(&r.comp->ppu) == 0 && ps3_spurs_runtime_shutdown(&r.comp->runtime) == 0);
}
// M3 lanes over the real registries: pre-effect trace capacity, and traced rollback of a created lwmutex.
void run_sync_trace_lanes() {
    long mutex_index = -1, cond_index = -1;
    {   // probe with ample capacity: where the W22/W23 events sit in the published order
        Runtime r;
        r.spu_initialize(6, 0); r.attribute(5, 128, 249); r.call_initialize(INSTANCE1);
        // nth 0 at each address is the W-table zero-fill of the 24 B span; nth 1 is the sync registry's own write.
        mutex_index = find_hle_write(r, INSTANCE1 + W22_LWMUTEX, 1); cond_index = find_hle_write(r, INSTANCE1 + W23_LWCOND, 1);
        CHECK(mutex_index >= 0 && cond_index > mutex_index);
        PpuMemoryTraceSample sample;
        CHECK(r.recorder.read(size_t(mutex_index), sample) && sample.event.length == 24 && sample.event.byte_transfer);
        CHECK(r.recorder.read(size_t(cond_index), sample) && sample.event.length == 8);
    }
    if (mutex_index < 0 || cond_index <= mutex_index) return;
    for (int lane = 0; lane < 2; ++lane) {   // 0: no room for the lwmutex image, 1: room for it but not the lwcond image
        Runtime r;
        const size_t capacity = size_t(lane ? cond_index : mutex_index);
        CHECK(r.recorder.configure(r.events.data(), capacity, PpuMemoryTraceRetention::Strict, 0));
        r.spu_initialize(6, 0); r.attribute(5, 128, 249);
        const auto before = snapshot(r, INSTANCE1, K1_SPAN);
        r.call_initialize(INSTANCE1);
        auto &c = *r.control;
        // Terminal explicit stop BEFORE the guest effect that cannot be recorded: never a silent untraced write.
        CHECK(c.stop == StartupStop::TraceCapacity && c.spurs_initialize_returns == 0 && c.spurs_write_trace_failure);
        CHECK(untraced_bytes(r, INSTANCE1, before, false) == 0);
        CHECK(live_mutexes(r) == size_t(lane) && live_conditions(r) == 0);
        // the refused trace reservation is itself recorded, and the registry was NOT called for the refused operation
        CHECK(c.spurs_sync_create_calls == size_t(lane + 1) && c.spurs_sync_has_last_failed && c.spurs_sync_last_failed.sequence == uint32_t(lane + 1));
        const auto &refused = c.spurs_sync_create[lane];
        CHECK(refused.operation == (lane ? StartupSpursSyncOperation::LwcondCreate : StartupSpursSyncOperation::LwmutexCreate));
        CHECK(!refused.real_call_performed && refused.native_status == -1 && refused.mapped_rc == uint32_t(PS3_LV2_PROVIDER_FAILURE) && !refused.mutexes.taken && !refused.conditions.taken);
        CHECK(lane == 0 || (c.spurs_sync_create[0].real_call_performed && c.spurs_sync_create[0].native_status == PS3_LWMUTEX_OK));
        const auto after = snapshot(r, INSTANCE1 + W22_LWMUTEX, 32);
        bool mutex_written = false, cond_written = false;
        for (size_t i = 0; i < 24; ++i) mutex_written |= after[i] != 0;
        for (size_t i = 24; i < 32; ++i) cond_written |= after[i] != 0;
        CHECK(mutex_written == (lane == 1) && !cond_written);
    }
    {   // lwcond registry exhausted -> EAGAIN: the lwmutex created at S7 is destroyed; image + DEAD pair are both traced
        Runtime r;
        const unsigned char name[PS3_LWCOND_NAME_BYTES] = {'f', 'i', 'l', 'l', 0, 0, 0, 0};
        CHECK(ps3_lwmutex_create(&r.mutexes, 0x7D1400, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE).status == PS3_LWMUTEX_OK);
        for (uint32_t i = 0; i < 4; ++i) {
            ps3_lwcond_report report{};
            CHECK(ps3_lwcond_create(&r.conditions, 0x7D1440 + 8 * i, 0x7D1400, name, &report).status == PS3_LWCOND_OK);
        }
        r.spu_initialize(6, 0); r.attribute(5, 128, 249);
        const auto before = snapshot(r, INSTANCE1, K1_SPAN);
        r.call_initialize(INSTANCE1);
        auto &c = *r.control;
        CHECK(c.stop == StartupStop::None && c.spurs_initialize_returns == 1 && c.spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_FAILED);
        CHECK(uint32_t(r.execution.ppu.cpu.gpr[3]) == uint32_t(PS3_LV2_EAGAIN));
        CHECK(live_mutexes(r) == 1 && live_conditions(r) == 4);   // only the fixture objects remain
        CHECK(untraced_bytes(r, INSTANCE1, before, false) == 0);
        PpuMemoryTraceSample create{}, retire{};
        // at W22: zero-fill (nth 0), registry create image (nth 1), rollback destroy pair (nth 2), nothing else
        const long first = find_hle_write(r, INSTANCE1 + W22_LWMUTEX, 1), second = find_hle_write(r, INSTANCE1 + W22_LWMUTEX, 2);
        CHECK(find_hle_write(r, INSTANCE1 + W22_LWMUTEX, 0) >= 0 && first > 0 && second > first && find_hle_write(r, INSTANCE1 + W22_LWMUTEX, 3) < 0);
        CHECK(r.recorder.read(size_t(first), create) && create.event.length == 24 && r.recorder.read(size_t(second), retire) && retire.event.length == 8);
        const auto image = snapshot(r, INSTANCE1 + W22_LWMUTEX, 8);
        CHECK(!std::memcmp(retire.event.bytes, image.data(), 8) && image[0] == 0xFF && image[4] == 0);   // owner = DEAD, waiters = 0
    }
}
// R7: the real N12 startup allocates 0x500000 through the guest's own allocator BEFORE aa6269a8, so the quota TOTAL
// after one successful SPURS initialize is 0x64C000, not the 0x14C000 fixture total. The legacy log predicate
// (quota.used == 0x14C000) was false despite a valid return. The SPURS charge is the measured before/after delta.
void run_prior_allocation_quota() {
    constexpr uint64_t PRIOR = UINT64_C(0x500000);                        // 5 MiB, 1 MiB-aligned
    constexpr uint64_t SPURS_FIRST = UINT64_C(5) * 0x40000 + 0x8000 + 0x4000;   // hand-derived: 5 LS + handler + kernel stacks
    constexpr uint64_t BIG_QUOTA = UINT64_C(0x6C0000);                   // == window [0x100000,0x7C0000); quota may not exceed it
    static_assert(SPURS_FIRST == STARTUP_SPURS_FIRST_INSTANCE_CHARGE, "hand-derived charge must equal the shared constant");
    Runtime r(1, true, true, 0x100000, 0x7C0000, BIG_QUOTA, 4);
    uint32_t prior_address = 0;
    const auto allocated = ps3_user_memory_allocate(&r.memory, PRIOR, PS3_USER_MEMORY_ALIGN_1M, &prior_address);   // the real allocator
    CHECK(allocated.status == PS3_USER_MEMORY_OK && prior_address == 0x100000 && r.used() == PRIOR);
    r.spu_initialize(6, 0);
    r.attribute(5, 128, 249);
    r.call_initialize(INSTANCE1);
    auto &c = *r.control;
    CHECK(c.stop == StartupStop::None && c.spurs_initialize_calls == 1 && c.spurs_initialize_returns == 1 && c.spurs_initialize_has_result);
    CHECK(c.spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_OK && c.spurs_initialize_result.rc == 0 && r.execution.ppu.cpu.gpr[3] == 0);
    // the old assertion is false for a valid initialize: this is the reproduced bug, not a weakened expectation
    const bool legacy_total_predicate = r.used() == 0x14C000;
    CHECK(!legacy_total_predicate && r.used() == PRIOR + SPURS_FIRST);
    // statuses, before/after samples, delta and total are all recorded separately
    const auto &q = c.spurs_quota;
    CHECK(q.before.status == PS3_USER_MEMORY_OK && q.after.status == PS3_USER_MEMORY_OK);
    CHECK(q.before.quota == BIG_QUOTA && q.after.quota == BIG_QUOTA);
    CHECK(q.before.used == PRIOR && q.before.available == BIG_QUOTA - PRIOR && q.before.allocations == 1);
    CHECK(q.after.used == PRIOR + SPURS_FIRST && q.after.available == BIG_QUOTA - PRIOR - SPURS_FIRST);
    CHECK(q.measured && q.charged && q.prior_used == PRIOR && q.delta_charge == SPURS_FIRST && q.total_used == PRIOR + SPURS_FIRST);
    CHECK(startup_spurs_quota_charge_is(q, SPURS_FIRST));          // the shared predicate the N12 boundary line uses
    CHECK(!startup_spurs_quota_charge_is(q, q.total_used));        // the total is NOT the charge
    CHECK(!startup_spurs_quota_charge_is(q, SPURS_FIRST + 0x10000) && !startup_spurs_quota_charge_is(q, SPURS_FIRST - 0x4000));
    // a diagnostic query after the call agrees with the "after" sample (nothing else charged in between)
    const auto final_sample = startup_quota_sample(&r.memory);
    CHECK(final_sample.status == PS3_USER_MEMORY_OK && final_sample.used == q.after.used && final_sample.available == q.after.available);
    // shutdown returns exactly the SPURS charge; the prior guest allocation remains owned by the guest
    CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_OK && r.used() == PRIOR);
    ps3_user_memory_info info{};
    CHECK(ps3_user_memory_query(&r.memory, &info) == PS3_USER_MEMORY_OK && info.allocation_count == 1);
}
// The shared report calculation on synthetic samples: it must refuse every pair that is not a conserved measurement
// of growth, and must never derive a charge from a bare total (the shape of the original bug).
void run_quota_report_calculation() {
    auto sample = [](int status, uint64_t quota, uint64_t used, size_t allocations = 1) {
        StartupQuotaSample s; s.status = status; s.quota = quota; s.used = used; s.available = quota - used; s.allocations = allocations; return s; };
    const uint64_t Q = 0x6C0000, PRIOR = 0x500000, D = 0x14C000;
    const auto good = startup_spurs_quota_report(sample(0, Q, PRIOR), sample(0, Q, PRIOR + D));
    CHECK(good.measured && good.charged && good.prior_used == PRIOR && good.delta_charge == D && good.total_used == PRIOR + D);
    CHECK(startup_spurs_quota_charge_is(good, D) && !startup_spurs_quota_charge_is(good, PRIOR + D) && !startup_spurs_quota_charge_is(good, 0));
    const auto empty = startup_spurs_quota_report(sample(0, Q, 0, 0), sample(0, Q, D, 0));
    CHECK(startup_spurs_quota_charge_is(empty, D) && empty.prior_used == 0 && empty.total_used == D);
    const auto zero = startup_spurs_quota_report(sample(0, Q, PRIOR), sample(0, Q, PRIOR));
    CHECK(zero.measured && zero.charged && zero.delta_charge == 0 && startup_spurs_quota_charge_is(zero, 0) && !startup_spurs_quota_charge_is(zero, D));
    // total only, no "before": exactly what the old log line had. Not a measurement; refused for any expected delta.
    const auto total_only = startup_spurs_quota_report(StartupQuotaSample{}, sample(0, Q, PRIOR + D));
    CHECK(!total_only.measured && !total_only.charged && total_only.delta_charge == 0 && !startup_spurs_quota_charge_is(total_only, D));
    CHECK(!startup_spurs_quota_charge_is(startup_spurs_quota_report(sample(0, Q, PRIOR), StartupQuotaSample{}), D));
    CHECK(!startup_spurs_quota_charge_is(startup_spurs_quota_report(sample(2, Q, PRIOR), sample(0, Q, PRIOR + D)), D));     // before query failed
    CHECK(!startup_spurs_quota_charge_is(startup_spurs_quota_report(sample(0, Q, PRIOR), sample(1, Q, PRIOR + D)), D));     // after query failed
    CHECK(!startup_spurs_quota_charge_is(startup_spurs_quota_report(sample(0, Q, PRIOR), sample(0, Q + 0x10000, PRIOR + D)), D));   // quota changed
    auto broken = sample(0, Q, PRIOR + D); broken.available += 0x10000;                                                     // used+available != quota
    CHECK(!startup_spurs_quota_charge_is(startup_spurs_quota_report(sample(0, Q, PRIOR), broken), D));
    const auto shrink = startup_spurs_quota_report(sample(0, Q, PRIOR + D), sample(0, Q, PRIOR));                           // freed, not charged
    CHECK(shrink.measured && !shrink.charged && shrink.delta_charge == 0 && !startup_spurs_quota_charge_is(shrink, D) && !startup_spurs_quota_charge_is(shrink, 0));
    CHECK(startup_quota_sample(nullptr).status == STARTUP_QUOTA_UNOBSERVED);
    // an execution whose composition never ran has no observation port: the report stays unmeasured, never waived
    Runtime r(1, false);
    CHECK(!r.execution.spurs_quota_memory && !r.control->spurs_quota.measured && r.control->spurs_quota.before.status == STARTUP_QUOTA_UNOBSERVED);
}
void run_negatives() {
    {   // composition absent: the dispatch is a terminal MissingServices stop, no effect, no fake rc
        Runtime r(1, false);
        r.attribute(5, 128, 249);
        unsigned char before[0x1000]; r.mem().read(r.mem().context, INSTANCE1, before, sizeof before);
        r.call_initialize(INSTANCE1);
        CHECK(r.control->stop == StartupStop::MissingServices && r.control->spurs_initialize_calls == 1 && r.control->spurs_initialize_returns == 0);
        unsigned char after[0x1000]; r.mem().read(r.mem().context, INSTANCE1, after, sizeof after);
        CHECK(!std::memcmp(before, after, sizeof before) && r.used() == 0);
    }
    {   // NULL attribute: guest error rc without any resource, then a clean second composition refusal
        Runtime r;
        r.spu_initialize(6, 0);
        auto &cpu = r.execution.ppu.cpu;
        cpu.gpr[3] = INSTANCE1; cpu.gpr[4] = 0; cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        r.control->stop = StartupStop::None; active = r.control.get(); r.control->cpu = &cpu;
        try { ps3_hle_call(0xaa6269a8, &cpu); active = nullptr; } catch (const Terminal &) {}
        CHECK(r.control->stop == StartupStop::None && uint32_t(cpu.gpr[3]) == 0x80410711u && r.used() == 0);
        CHECK(r.control->spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_FAILED);
        // a refused initialize is measured too, with a zero delta; it must never satisfy the charge predicate
        CHECK(r.control->spurs_quota.measured && r.control->spurs_quota.charged && r.control->spurs_quota.delta_charge == 0);
        CHECK(!startup_spurs_quota_charge_is(r.control->spurs_quota, STARTUP_SPURS_FIRST_INSTANCE_CHARGE));
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, r.ports) == StartupSpursComposeStatus::AlreadyComposed);
    }
    {   // unsupported attribute (flags != 0): terminal native STOP, never a guest error, TOC/r3 untouched, nothing allocated
        Runtime r;
        r.spu_initialize(6, 0);
        r.attribute(5, 128, 249, 1);
        unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8);
        r.call_initialize(INSTANCE1);
        CHECK(r.control->stop == StartupStop::SpursInitializeStop && r.control->spurs_initialize_returns == 0);
        CHECK(r.control->spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_STOP && r.control->spurs_initialize_result.reason == PS3_SPURS_STOP_UNSUPPORTED_ATTRIBUTE);
        unsigned char toc[8]; r.mem().read(r.mem().context, TOC_SLOT, toc, 8);
        CHECK(!std::memcmp(toc, sentinel, 8) && r.used() == 0);
    }
    {   // shutdown failure prevents release: handler join reports failure -> BUSY, nothing released, retry succeeds
        Runtime r;
        r.spu_initialize(6, 0);
        r.attribute(5, 128, 249);
        r.call_initialize(INSTANCE1);
        CHECK(r.control->stop == StartupStop::None && r.used() == 0x14C000);
        r.threads[0][PS3_SPURS_THREAD_HANDLER].fail_next_join = 1;
        CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_BUSY);
        CHECK(r.execution.ppu.admitted && r.used() == 0x14C000 && r.execution.spurs->instances[0].state != PS3_SPURS_INSTANCE_FREE);
        CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_OK && r.used() == 0);
    }
    {   // composition refusals leave execution.spurs unset
        Runtime r(1, false);
        StartupSpursPorts bad = r.ports; bad.ppu_guard = bad.lv2_guard;
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, bad) == StartupSpursComposeStatus::GuardsNotDistinct && !r.execution.spurs && !r.execution.event_queues);
        bad = r.ports; bad.local_stores[3] = nullptr;
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, bad) == StartupSpursComposeStatus::InvalidArgument && !r.execution.spurs);
        ps3_spu_resource_policy other{};
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, other, r.ports) == StartupSpursComposeStatus::InvalidArgument && !r.execution.spurs);
        r.execution.spu_resources = nullptr;
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, r.ports) == StartupSpursComposeStatus::PolicyMismatch && !r.execution.spurs);
        r.execution.spu_resources = &r.policy;
        CHECK(!r.comp->lv2.initialized && !r.comp->ppu.initialized);   // refusals happen before any effect
        r.execution.conditions = nullptr;
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, r.ports) == StartupSpursComposeStatus::MissingConditions && !r.execution.spurs);
        r.execution.conditions = &r.conditions;
        r.execution.ppu.cpu.thread_id = 0;
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, r.ports) == StartupSpursComposeStatus::NotAdmitted && !r.execution.spurs);
        r.execution.ppu.cpu.thread_id = 1;
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, r.ports) == StartupSpursComposeStatus::Ok && r.execution.spurs);
        r.composed = true;
    }
    {   // unadmitted execution is refused before any effect
        Runtime r(1, false, false);
        CHECK(startup_spurs_compose(r.execution, *r.comp, r.domain, r.memory, r.policy, r.ports) == StartupSpursComposeStatus::NotAdmitted && !r.execution.spurs);
        CHECK(!r.comp->lv2.initialized && !r.comp->ppu.initialized);
    }
}
}
// ---- SPURS synchronization create diagnostics (DIAGNOSTIC observation only; see StartupSpursSyncCreateRecord) -------------
// These cases reproduce the diagnostic MEANING over the real adapter and the real registries: which operation failed,
// the native status, and a pre-call occupancy snapshot that tells an exhausted object table from exhausted binding credits.
// They do not establish what happened in any sandbox run.
constexpr uint32_t SYNC_M0 = 0x7D1400, SYNC_M_STRIDE = 0x40, SYNC_M_NEW = 0x7D1500, SYNC_C0 = 0x7D1600, SYNC_C_STRIDE = 8;
using SyncRecord = StartupSpursSyncCreateRecord;
using SyncSnapshot = StartupSpursSyncRegistrySnapshot;
void sync_fixture_mutex(Runtime &r, uint32_t address) {
    CHECK(ps3_lwmutex_create(&r.mutexes, address, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE).status == PS3_LWMUTEX_OK);
}
void sync_fixture_cond(Runtime &r, uint32_t address, uint32_t mutex) {
    static const unsigned char name[PS3_LWCOND_NAME_BYTES] = {'f', 'i', 'x', 0, 0, 0, 0, 0};
    ps3_lwcond_report report{};
    CHECK(ps3_lwcond_create(&r.conditions, address, mutex, name, &report).status == PS3_LWCOND_OK);
}
size_t sync_pins_active(Runtime &r) {
    const auto guard = tracked_guard_port(&r.g_dom);
    guard.lock(guard.context);
    size_t n = 0;
    for (const auto &pin : r.pins) n += pin.active != 0;
    guard.unlock(guard.context);
    return n;
}
bool snapshot_is(const SyncSnapshot &s, size_t capacity, size_t active, size_t reusable, size_t exhausted_generation = 0) {
    return s.taken && s.object_capacity == capacity && s.active_objects == active && s.reusable_objects == reusable &&
           s.exhausted_generation_objects == exhausted_generation && active + reusable + exhausted_generation == capacity;
}
bool bindings_are(const SyncSnapshot &s, size_t count, size_t capacity) {
    return s.condition_binding_count == count && s.condition_binding_capacity == capacity;
}
// Only the two synchronization guards, in acquisition order: 'M'/'C' followed by how many tracked guards were already held.
std::string sync_guard_order(Runtime &r) {
    std::string order;
    for (size_t i = 0; i < guard_log_count; ++i) {
        const char tag = guard_log[i].guard == &r.g_mutex ? 'M' : guard_log[i].guard == &r.g_cond ? 'C' : 0;
        if (tag) { order += tag; order += char('0' + guard_log[i].depth_before); order += ' '; }
    }
    return order;
}
std::string sync_line(const char *label, const SyncRecord &record) {
    char line[1536];
    const size_t length = startup_spurs_sync_create_format(line, sizeof line, label, record);
    CHECK(length > 0 && length < sizeof line && std::strlen(line) == length);
    return line;
}
bool line_has(const std::string &line, const char *text) {
    const bool found = line.find(text) != std::string::npos;
    if (!found) std::printf("SPURS_INITIALIZE_BRIDGE_LINE_MISSING text=%s\n", text);
    return found;
}
void run_sync_create_both_available() {
    Runtime r(1, false);
    const auto port = native_startup_spurs_sync_port(&r.execution);
    const uint32_t mutex_queue = r.mutexes.next_queue_id, cond_queue = r.conditions.next_queue_id;
    guard_log_reset();
    CHECK(port.lwmutex_create(port.context, SYNC_M0, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE) == 0);
    // Each registry is sampled alone under its own guard and released BEFORE the real create takes the mutex guard again.
    CHECK(sync_guard_order(r) == "M0 C0 M0 " && nested_same_guard_acquisitions == 0 && thread_holds_no_tracked_guard());
    guard_log_reset();
    CHECK(port.lwcond_create(port.context, SYNC_C0, SYNC_M0) == 0);
    CHECK(sync_guard_order(r) == "M0 C0 C0 M1 " && nested_same_guard_acquisitions == 0 && thread_holds_no_tracked_guard());   // M1: the registries' own bind under the cond guard
    auto &c = *r.control;
    CHECK(c.spurs_sync_create_calls == 2 && c.spurs_sync_create_retained == 2 && c.spurs_sync_create_dropped == 0);
    CHECK(!c.spurs_sync_create_overflow && !c.spurs_sync_has_last_failed);
    const SyncRecord &m = c.spurs_sync_create[0], &k = c.spurs_sync_create[1];
    CHECK(m.operation == StartupSpursSyncOperation::LwmutexCreate && m.sequence == 1 && m.address == SYNC_M0);
    CHECK(m.protocol == PS3_LWMUTEX_FIFO && m.recursive == PS3_LWMUTEX_RECURSIVE && m.real_call_performed);
    CHECK(m.native_status == PS3_LWMUTEX_OK && m.memory_status == PS3_GUEST_MEMORY_OK && m.mapped_rc == 0 && m.nested_mutex_status == -1);
    CHECK(snapshot_is(m.mutexes, 4, 0, 4) && bindings_are(m.mutexes, 0, 4) && m.mutexes.next_queue_id == mutex_queue && m.mutexes.queue_id_available);
    CHECK(m.mutexes.overlap_evaluated && m.mutexes.overlapping_objects == 0 && !m.conditions.overlap_evaluated);
    CHECK(snapshot_is(m.conditions, 4, 0, 4) && m.conditions.next_queue_id == cond_queue && m.conditions.queue_id_available);
    CHECK(k.operation == StartupSpursSyncOperation::LwcondCreate && k.sequence == 2 && k.address == SYNC_C0 && k.mutex_address == SYNC_M0);
    CHECK(k.real_call_performed && k.native_status == PS3_LWCOND_OK && k.memory_status == PS3_GUEST_MEMORY_OK && k.mapped_rc == 0);
    CHECK(k.publication_count == 1);
    // pre-call: the lwmutex just created is visible, its binding credit is not yet consumed by this condition
    CHECK(snapshot_is(k.mutexes, 4, 1, 3) && bindings_are(k.mutexes, 0, 4));
    CHECK(snapshot_is(k.conditions, 4, 0, 4) && k.conditions.overlap_evaluated && k.conditions.overlapping_objects == 0 && !k.mutexes.overlap_evaluated);
    CHECK(r.mutexes.condition_binding_count == 1 && live_mutexes(r) == 1 && live_conditions(r) == 1 && sync_pins_active(r) == 2);
    // destroy callbacks (the unwind) never write the record
    CHECK(port.lwcond_destroy(port.context, SYNC_C0) == 0 && port.lwmutex_destroy(port.context, SYNC_M0) == 0);
    CHECK(c.spurs_sync_create_calls == 2 && c.spurs_sync_create_retained == 2 && !c.spurs_sync_has_last_failed && c.spurs_sync_create[1].sequence == 2);
}
void run_sync_create_mutex_objects_full() {
    Runtime r(1, false);
    for (uint32_t i = 0; i < 4; ++i) sync_fixture_mutex(r, SYNC_M0 + SYNC_M_STRIDE * i);
    const auto port = native_startup_spurs_sync_port(&r.execution);
    const size_t pins = sync_pins_active(r);
    const auto before = snapshot(r, SYNC_M_NEW, 24);
    CHECK(port.lwmutex_create(port.context, SYNC_M_NEW, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE) == uint32_t(PS3_LV2_EAGAIN));
    auto &c = *r.control;
    CHECK(c.spurs_sync_create_calls == 1 && c.spurs_sync_create_retained == 1 && c.spurs_sync_has_last_failed && !c.spurs_sync_create_overflow);
    const SyncRecord &m = c.spurs_sync_create[0];
    CHECK(m.operation == StartupSpursSyncOperation::LwmutexCreate && m.real_call_performed && m.mapped_rc == uint32_t(PS3_LV2_EAGAIN));
    CHECK(m.native_status == PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    CHECK(snapshot_is(m.mutexes, 4, 4, 0) && bindings_are(m.mutexes, 0, 4) && m.mutexes.queue_id_available);   // object table full, queue ids and bindings are not the reason
    CHECK(snapshot_is(m.conditions, 4, 0, 4));
    // the real registry answers the same way, and the diagnostic carries ITS memory status, not an invented one
    const auto again = ps3_lwmutex_create(&r.mutexes, SYNC_M_NEW, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE);
    CHECK(again.status == PS3_LWMUTEX_RESOURCE_EXHAUSTED && m.memory_status == int(again.memory_status));
    CHECK(c.spurs_sync_last_failed.sequence == 1 && sync_line("x", c.spurs_sync_last_failed) == sync_line("x", m));
    CHECK(live_mutexes(r) == 4 && sync_pins_active(r) == pins && snapshot(r, SYNC_M_NEW, 24) == before);
}
void run_sync_create_condition_binding_credits_full() {
    Runtime r(1, false, true, WINDOW, WINDOW_END, QUOTA, 2, 1);   // one binding credit, four condition slots, four mutex slots
    sync_fixture_mutex(r, SYNC_M0);
    sync_fixture_cond(r, SYNC_C0, SYNC_M0);
    const auto port = native_startup_spurs_sync_port(&r.execution);
    const auto mutex_image = snapshot(r, SYNC_M0, 24), first_cond_image = snapshot(r, SYNC_C0, 8), second_before = snapshot(r, SYNC_C0 + SYNC_C_STRIDE, 8);
    const size_t pins = sync_pins_active(r);
    const uint32_t second = SYNC_C0 + SYNC_C_STRIDE;
    CHECK(port.lwcond_create(port.context, second, SYNC_M0) == uint32_t(PS3_LV2_EAGAIN));
    auto &c = *r.control;
    CHECK(c.spurs_sync_create_calls == 1 && c.spurs_sync_has_last_failed);
    const SyncRecord &k = c.spurs_sync_create[0];
    CHECK(k.operation == StartupSpursSyncOperation::LwcondCreate && k.real_call_performed && k.mapped_rc == uint32_t(PS3_LV2_EAGAIN));
    CHECK(k.native_status == PS3_LWCOND_RESOURCE_EXHAUSTED && k.publication_count == 0);
    // free mutex object, free condition slots, no overlap: the ONLY full resource is the binding credit table
    CHECK(snapshot_is(k.mutexes, 4, 1, 3) && bindings_are(k.mutexes, 1, 1) && k.mutexes.queue_id_available);
    CHECK(snapshot_is(k.conditions, 4, 1, 3) && k.conditions.queue_id_available && k.conditions.overlap_evaluated && k.conditions.overlapping_objects == 0);
    static const unsigned char probe_name[PS3_LWCOND_NAME_BYTES] = {'p', 'r', 'o', 'b', 'e', 0, 0, 0};
    ps3_lwcond_report probe_report{};
    const auto again = ps3_lwcond_create(&r.conditions, second, SYNC_M0, probe_name, &probe_report);
    CHECK(again.status == PS3_LWCOND_RESOURCE_EXHAUSTED && k.memory_status == int(again.memory_status) && k.nested_mutex_status == int(again.mutex_status));
    // real rollback: nothing leaked, the first graph is untouched
    CHECK(r.mutexes.condition_binding_count == 1 && live_mutexes(r) == 1 && live_conditions(r) == 1 && sync_pins_active(r) == pins);
    CHECK(snapshot(r, SYNC_M0, 24) == mutex_image && snapshot(r, SYNC_C0, 8) == first_cond_image && snapshot(r, second, 8) == second_before);
    // freeing the first condition makes the credit and the address usable again; the failed record is kept
    CHECK(port.lwcond_destroy(port.context, SYNC_C0) == 0);
    CHECK(port.lwcond_create(port.context, second, SYNC_M0) == 0 && r.mutexes.condition_binding_count == 1 && sync_pins_active(r) == pins);
    CHECK(c.spurs_sync_create_calls == 2 && c.spurs_sync_create[1].native_status == PS3_LWCOND_OK && bindings_are(c.spurs_sync_create[1].mutexes, 0, 1));
    CHECK(c.spurs_sync_has_last_failed && c.spurs_sync_last_failed.sequence == 1 && c.spurs_sync_last_failed.native_status == PS3_LWCOND_RESOURCE_EXHAUSTED);
    // the report line says it in plain scalars
    const std::string line = sync_line("last_failed", c.spurs_sync_last_failed);
    CHECK(line_has(line, "STARTUP_SPURS_SYNC_CREATE ") && line_has(line, "slot=last_failed") && line_has(line, "operation=lwcond_create"));
    CHECK(line_has(line, "mapped_rc=0x80010001") && line_has(line, "call_performed=1") && line_has(line, "gameplay_claim=false") && line_has(line, "snapshot_atomic=false"));
    CHECK(line_has(line, "mtx_binding_count=1") && line_has(line, "mtx_binding_capacity=1") && line_has(line, "mtx_active=1") && line_has(line, "cnd_active=1"));
    CHECK(line_has(line, "cnd_capacity=4") && line_has(line, "cnd_reusable=3") && line_has(line, "mutex_address=0x007d1400") && line_has(line, "address=0x007d1608"));
    char tiny[24];
    const size_t full = startup_spurs_sync_create_format(tiny, sizeof tiny, "t", c.spurs_sync_last_failed);
    CHECK(full == line.size() + std::strlen("slot=t") - std::strlen("slot=last_failed") && std::strlen(tiny) == sizeof tiny - 1);   // bounded and terminated
    CHECK(startup_spurs_sync_create_format(nullptr, 0, "t", c.spurs_sync_last_failed) == full);
}
void run_sync_create_condition_objects_full() {
    Runtime r(1, false, true, WINDOW, WINDOW_END, QUOTA, 2, 8);   // plenty of binding credits: only the condition table can be full
    sync_fixture_mutex(r, SYNC_M0);
    for (uint32_t i = 0; i < 4; ++i) sync_fixture_cond(r, SYNC_C0 + SYNC_C_STRIDE * i, SYNC_M0);
    const auto port = native_startup_spurs_sync_port(&r.execution);
    CHECK(port.lwcond_create(port.context, SYNC_C0 + 4 * SYNC_C_STRIDE, SYNC_M0) == uint32_t(PS3_LV2_EAGAIN));
    const SyncRecord &k = r.control->spurs_sync_create[0];
    CHECK(k.native_status == PS3_LWCOND_RESOURCE_EXHAUSTED && k.mapped_rc == uint32_t(PS3_LV2_EAGAIN));
    CHECK(snapshot_is(k.conditions, 4, 4, 0) && snapshot_is(k.mutexes, 4, 1, 3) && bindings_are(k.mutexes, 4, 8));
    CHECK(r.mutexes.condition_binding_count == 4 && live_conditions(r) == 4 && sync_pins_active(r) == 5);
}
void run_sync_create_provider_errors_are_not_resource_exhaustion() {
    Runtime r(1, false);
    sync_fixture_mutex(r, SYNC_M0);
    const auto port = native_startup_spurs_sync_port(&r.execution);
    const uint32_t PROVIDER = uint32_t(PS3_LV2_PROVIDER_FAILURE);
    CHECK(port.lwmutex_create(port.context, SYNC_M0 + 4, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE) == PROVIDER);   // misaligned
    CHECK(port.lwmutex_create(port.context, SYNC_M0, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE) == PROVIDER);       // overlaps a live object
    CHECK(port.lwcond_create(port.context, SYNC_M0 + 8, SYNC_M0) == PROVIDER);                                    // condition image inside its mutex
    CHECK(port.lwcond_create(port.context, SYNC_C0, SYNC_M0 + 0x400) == PROVIDER);                                // mutex does not exist
    auto &c = *r.control;
    CHECK(c.spurs_sync_create_calls == 4 && c.spurs_sync_create_retained == 4 && !c.spurs_sync_create_overflow);
    for (size_t i = 0; i < 4; ++i) {
        CHECK(c.spurs_sync_create[i].real_call_performed && c.spurs_sync_create[i].mapped_rc == PROVIDER && c.spurs_sync_create[i].sequence == i + 1);
        const int exhausted = i < 2 ? int(PS3_LWMUTEX_RESOURCE_EXHAUSTED) : int(PS3_LWCOND_RESOURCE_EXHAUSTED);
        CHECK(c.spurs_sync_create[i].native_status != exhausted && c.spurs_sync_create[i].native_status > 0);   // classified by the registry, never fabricated
    }
    CHECK(c.spurs_sync_create[0].native_status == PS3_LWMUTEX_INVALID_ARGUMENT && c.spurs_sync_create[1].native_status == PS3_LWMUTEX_ALREADY_EXISTS);
    CHECK(c.spurs_sync_create[2].native_status == PS3_LWCOND_INVALID_ARGUMENT);
    // the overlap probe sees the live object for the registry the operation targets (and says so), others are not evaluated
    CHECK(c.spurs_sync_create[1].mutexes.overlap_evaluated && c.spurs_sync_create[1].mutexes.overlapping_objects == 1);
    CHECK(c.spurs_sync_create[0].mutexes.overlapping_objects == 1 && snapshot_is(c.spurs_sync_create[3].mutexes, 4, 1, 3) && bindings_are(c.spurs_sync_create[3].mutexes, 0, 4));
    CHECK(c.spurs_sync_has_last_failed && c.spurs_sync_last_failed.sequence == 4);
    CHECK(live_mutexes(r) == 1 && live_conditions(r) == 0 && r.mutexes.condition_binding_count == 0 && sync_pins_active(r) == 1);
}
void run_sync_create_overflow_is_bounded_and_inert() {
    Runtime r(1, false);
    const auto port = native_startup_spurs_sync_port(&r.execution);
    uint32_t rc[6];
    for (uint32_t i = 0; i < 4; ++i) rc[i] = port.lwmutex_create(port.context, SYNC_M0 + SYNC_M_STRIDE * i, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE);
    rc[4] = port.lwmutex_create(port.context, SYNC_M_NEW, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE);   // table full: EAGAIN, record is over capacity
    rc[5] = port.lwmutex_create(port.context, SYNC_M_NEW + 4, PS3_LWMUTEX_FIFO, PS3_LWMUTEX_RECURSIVE);   // misaligned, over capacity again
    auto &c = *r.control;
    for (int i = 0; i < 4; ++i) CHECK(rc[i] == 0);
    CHECK(rc[4] == uint32_t(PS3_LV2_EAGAIN) && rc[5] == uint32_t(PS3_LV2_PROVIDER_FAILURE));   // the diagnostic never changes a return code
    CHECK(c.spurs_sync_create_calls == 6 && c.spurs_sync_create_retained == STARTUP_SPURS_SYNC_CREATE_CAPACITY && c.spurs_sync_create_dropped == 2);
    CHECK(c.spurs_sync_create_overflow && c.spurs_sync_has_last_failed);
    for (size_t i = 0; i < STARTUP_SPURS_SYNC_CREATE_CAPACITY; ++i)   // the retained records are the first four, untouched by the overflow
        CHECK(c.spurs_sync_create[i].sequence == i + 1 && c.spurs_sync_create[i].native_status == PS3_LWMUTEX_OK && snapshot_is(c.spurs_sync_create[i].mutexes, 4, i, 4 - i));
    CHECK(c.spurs_sync_last_failed.sequence == 6 && c.spurs_sync_last_failed.native_status == PS3_LWMUTEX_INVALID_ARGUMENT && snapshot_is(c.spurs_sync_last_failed.mutexes, 4, 4, 0));
    CHECK(live_mutexes(r) == 4 && sync_pins_active(r) == 4 && thread_holds_no_tracked_guard());   // nothing leaked, nothing held
    char summary[256];
    const size_t length = startup_spurs_sync_create_format_summary(summary, sizeof summary, c.spurs_sync_create_calls, c.spurs_sync_create_retained,
        c.spurs_sync_create_dropped, c.spurs_sync_create_overflow, c.spurs_sync_has_last_failed);
    const std::string text = summary;
    CHECK(length == text.size() && line_has(text, "STARTUP_SPURS_SYNC_CREATE_SUMMARY ") && line_has(text, "calls=6") && line_has(text, "retained=4") &&
          line_has(text, "dropped=2") && line_has(text, "overflow=1") && line_has(text, "last_failed_present=1") && line_has(text, "capacity=4"));
    for (int i = 0; i < 4; ++i) CHECK(port.lwmutex_destroy(port.context, SYNC_M0 + SYNC_M_STRIDE * i) == 0);
    CHECK(c.spurs_sync_create_calls == 6 && c.spurs_sync_last_failed.sequence == 6);
}
// End to end through the real aa6269a8 transaction: the failure record exists BEFORE the unwind and survives it.
void run_sync_create_through_initialize() {
    {   // a successful first instance then a second one: four creates, all retained, pre-call occupancy grows
        Runtime r;
        r.spu_initialize(6, 0); r.attribute(5, 128, 249);
        r.call_initialize(INSTANCE1);
        r.attribute(1, 128, 249);
        r.call_initialize(INSTANCE2);
        auto &c = *r.control;
        CHECK(c.spurs_initialize_returns == 2 && r.execution.ppu.cpu.gpr[3] == 0);
        CHECK(c.spurs_sync_create_calls == 4 && c.spurs_sync_create_retained == 4 && !c.spurs_sync_create_overflow && !c.spurs_sync_has_last_failed);
        const StartupSpursSyncOperation expect[4] = {StartupSpursSyncOperation::LwmutexCreate, StartupSpursSyncOperation::LwcondCreate,
            StartupSpursSyncOperation::LwmutexCreate, StartupSpursSyncOperation::LwcondCreate};
        for (size_t i = 0; i < 4; ++i)
            CHECK(c.spurs_sync_create[i].operation == expect[i] && c.spurs_sync_create[i].real_call_performed && c.spurs_sync_create[i].mapped_rc == 0 && c.spurs_sync_create[i].sequence == i + 1);
        CHECK(c.spurs_sync_create[0].address == INSTANCE1 + W22_LWMUTEX && c.spurs_sync_create[1].address == INSTANCE1 + W23_LWCOND && c.spurs_sync_create[1].mutex_address == INSTANCE1 + W22_LWMUTEX);
        CHECK(c.spurs_sync_create[2].address == INSTANCE2 + W22_LWMUTEX && c.spurs_sync_create[3].address == INSTANCE2 + W23_LWCOND);
        CHECK(snapshot_is(c.spurs_sync_create[0].mutexes, 4, 0, 4) && snapshot_is(c.spurs_sync_create[1].mutexes, 4, 1, 3));
        CHECK(snapshot_is(c.spurs_sync_create[2].mutexes, 4, 1, 3) && bindings_are(c.spurs_sync_create[2].mutexes, 1, 4) && snapshot_is(c.spurs_sync_create[2].conditions, 4, 1, 3));
        CHECK(snapshot_is(c.spurs_sync_create[3].mutexes, 4, 2, 2) && bindings_are(c.spurs_sync_create[3].mutexes, 1, 4) && snapshot_is(c.spurs_sync_create[3].conditions, 4, 1, 3));
    }
    {   // binding credits are the only full resource: the lwmutex create succeeds, the lwcond create fails, then everything unwinds
        Runtime r(1, true, true, WINDOW, WINDOW_END, QUOTA, 2, 1);
        sync_fixture_mutex(r, SYNC_M0);
        sync_fixture_cond(r, SYNC_C0, SYNC_M0);
        r.spu_initialize(6, 0); r.attribute(5, 128, 249);
        r.call_initialize(INSTANCE1);
        auto &c = *r.control;
        CHECK(c.stop == StartupStop::None && c.spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_FAILED && uint32_t(r.execution.ppu.cpu.gpr[3]) == uint32_t(PS3_LV2_EAGAIN));
        CHECK(c.spurs_sync_create_calls == 2 && c.spurs_sync_create_retained == 2 && c.spurs_sync_has_last_failed);
        const SyncRecord &m = c.spurs_sync_create[0], &k = c.spurs_sync_create[1];
        CHECK(m.operation == StartupSpursSyncOperation::LwmutexCreate && m.native_status == PS3_LWMUTEX_OK && snapshot_is(m.mutexes, 4, 1, 3));
        CHECK(k.operation == StartupSpursSyncOperation::LwcondCreate && k.native_status == PS3_LWCOND_RESOURCE_EXHAUSTED && k.mapped_rc == uint32_t(PS3_LV2_EAGAIN));
        CHECK(snapshot_is(k.mutexes, 4, 2, 2) && bindings_are(k.mutexes, 1, 1) && snapshot_is(k.conditions, 4, 1, 3));
        CHECK(k.address == INSTANCE1 + W23_LWCOND && k.mutex_address == INSTANCE1 + W22_LWMUTEX);
        // the unwind destroyed the SPURS lwmutex (a destroy callback), yet the failed create is intact and is the last failure
        CHECK(live_mutexes(r) == 1 && live_conditions(r) == 1 && r.mutexes.condition_binding_count == 1 && c.spurs_sync_create_calls == 2);
        CHECK(c.spurs_sync_last_failed.sequence == 2 && sync_line("a", c.spurs_sync_last_failed) == sync_line("a", k));
    }
    {   // the lwmutex table is full: the first create fails and no condition create is ever attempted
        Runtime r;
        for (uint32_t i = 0; i < 4; ++i) sync_fixture_mutex(r, SYNC_M0 + SYNC_M_STRIDE * i);
        r.spu_initialize(6, 0); r.attribute(5, 128, 249);
        r.call_initialize(INSTANCE1);
        auto &c = *r.control;
        CHECK(c.stop == StartupStop::None && uint32_t(r.execution.ppu.cpu.gpr[3]) == uint32_t(PS3_LV2_EAGAIN));
        CHECK(c.spurs_sync_create_calls == 1 && c.spurs_sync_has_last_failed && c.spurs_sync_last_failed.sequence == 1);
        const SyncRecord &m = c.spurs_sync_create[0];
        CHECK(m.operation == StartupSpursSyncOperation::LwmutexCreate && m.native_status == PS3_LWMUTEX_RESOURCE_EXHAUSTED && m.mapped_rc == uint32_t(PS3_LV2_EAGAIN));
        CHECK(snapshot_is(m.mutexes, 4, 4, 0) && bindings_are(m.mutexes, 0, 4) && snapshot_is(m.conditions, 4, 0, 4) && live_mutexes(r) == 4 && live_conditions(r) == 0);
    }
}
// Guest sc128 over the REAL composed registry after the real N12 initialize: the new queue is allocated from the
// same namespace the SPURS services already own (next never-reused serial), stored big-endian, and independently
// releasable. Expected id derived from the lv2 id contract (base 0x8D000000 + 0x100 * serial), not captured.
void run_guest_event_queue_after_spurs() {
    Runtime r(1);
    r.spu_initialize(6, 0);
    r.attribute(5, 128, 249);
    unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8);
    r.call_initialize(INSTANCE1);
    CHECK(r.control->stop == StartupStop::None && r.execution.ppu.cpu.gpr[3] == 0);
    ps3_lv2_objects &lv2 = r.comp->lv2;
    const size_t owned_before = ps3_lv2_live_count(&lv2, PS3_LV2_KIND_QUEUE);
    CHECK(owned_before >= 1);   // the SPURS services own queues already
    const uint32_t serial = lv2.serial[PS3_LV2_KIND_QUEUE];
    CHECK(serial >= owned_before);
    const uint32_t expected_id = 0x8D000000u + 0x100u * serial, guest_attr = STACK_BASE + 0x400, guest_out = STACK_BASE + 0x440;
    for (uint32_t existing = 0; existing < serial; ++existing) {
        ps3_lv2_queue q{};
        CHECK(ps3_lv2_queue_snapshot(&lv2, 0x8D000000u + 0x100u * existing, &q) != PS3_LV2_OK || q.id != expected_id);
    }
    const unsigned char attr[16]{0, 0, 0, 2, 0, 0, 0, 1, 0, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E};   // PRIORITY, PPU
    r.put(guest_attr, attr, sizeof attr); r.put32(guest_out, 0xCAFEF00Du);
    auto &cpu = r.execution.ppu.cpu;
    cpu.gpr[11] = 128; cpu.gpr[3] = guest_out; cpu.gpr[4] = guest_attr; cpu.gpr[5] = 0; cpu.gpr[6] = 127;
    r.control->stop = StartupStop::None; active = r.control.get(); r.control->cpu = &cpu;
    try { lv2_syscall(&cpu); active = nullptr; } catch (const Terminal &) {}
    CHECK(r.control->stop == StartupStop::None && cpu.gpr[3] == 0);
    CHECK(r.get32(guest_out) == expected_id && r.control->event_queue_created_id == expected_id && r.control->event_queue_retained_id == 0);
    ps3_lv2_queue q{};
    CHECK(ps3_lv2_queue_snapshot(&lv2, expected_id, &q) == PS3_LV2_OK && q.protocol == 2 && q.type == 1 && q.size == 127 && q.active);
    CHECK(ps3_lv2_live_count(&lv2, PS3_LV2_KIND_QUEUE) == owned_before + 1);
    PpuMemoryTraceSample sample;
    const long at = find_hle_write(r, guest_out);
    CHECK(at >= 0 && r.recorder.read(size_t(at), sample) && sample.event.length == 4 && sample.event.value == expected_id);
    CHECK(r.control->event_queue_owned_count == 1 && r.control->event_queue_owned_ids[0] == expected_id);
    // Normal shutdown: the guest-created queue is released explicitly (the ledger empties and the id is gone).
    CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_OK);
    CHECK(r.control->event_queue_owned_count == 0 && ps3_lv2_queue_snapshot(&lv2, expected_id, &q) != PS3_LV2_OK);
}
// ---- Startup capacity profile (TARGET fixture mode: registries sized by startup_capacity_policy.h) -------------------------
// Observed native 0.24.11: 31 ordinary lwmutexes were live when the first SPURS initialize took the 32nd slot of 32 and the
// second SPURS initialize was refused solely for lack of a slot (the lwmutex object table, not conditions/bindings/queue ids).
// These cases use the REAL registries, domain, lv2/ppu registries and sync callbacks; no readiness or event is faked.
constexpr uint32_t PRIOR_MUTEXES = 31;
void make_prior_mutexes(Runtime &r, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) sync_fixture_mutex(r, SYNC_M0 + SYNC_M_STRIDE * i);   // 0x7D1400 + 0x40*i stays inside the committed 0x7CF000 range
}
void run_target_profile_second_spurs_after_31_prior_mutexes() {
    Runtime r(1, true, true, WINDOW, WINDOW_END, QUOTA, 2, 4, true);
    make_prior_mutexes(r, PRIOR_MUTEXES);
    CHECK(live_mutexes(r) == PRIOR_MUTEXES && live_conditions(r) == 0 && r.mutexes.condition_binding_count == 0);
    const size_t pins_prior = sync_pins_active(r);
    CHECK(pins_prior == PRIOR_MUTEXES);
    r.spu_initialize(6, 0); r.attribute(5, 128, 249);
    r.call_initialize(INSTANCE1);
    auto &c = *r.control;
    CHECK(c.stop == StartupStop::None && c.spurs_initialize_returns == 1 && c.spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_OK && r.execution.ppu.cpu.gpr[3] == 0);
    // the first SPURS instance takes the 32nd live lwmutex (31 prior + 1) and its lwcond
    CHECK(live_mutexes(r) == PRIOR_MUTEXES + 1 && live_conditions(r) == 1 && r.mutexes.condition_binding_count == 1 && sync_pins_active(r) == pins_prior + 2);
    CHECK(c.spurs_sync_create_calls == 2 && c.spurs_sync_create[0].native_status == PS3_LWMUTEX_OK && c.spurs_sync_create[1].native_status == PS3_LWCOND_OK);
    const size_t table = size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY);
    CHECK(snapshot_is(c.spurs_sync_create[0].mutexes, table, PRIOR_MUTEXES, table - PRIOR_MUTEXES));
    const auto first_mutex = snapshot(r, INSTANCE1 + W22_LWMUTEX, 24), first_cond = snapshot(r, INSTANCE1 + W23_LWCOND, 8);
    r.attribute(1, 128, 249);
    r.call_initialize(INSTANCE2);   // under the 32-slot BASELINE this is refused EAGAIN by the lwmutex object table
    CHECK(c.stop == StartupStop::None && c.spurs_initialize_calls == 2 && c.spurs_initialize_returns == 2);
    CHECK(c.spurs_initialize_result.outcome == PS3_SPURS_OUTCOME_OK && r.execution.ppu.cpu.gpr[3] == 0 && !c.spurs_sync_has_last_failed);
    std::printf("SPURS_INITIALIZE_BRIDGE_TARGET second_rc=0x%08x sync_calls=%zu last_failed=%d last_failed_native=%d mutex_capacity=%zu live_mutexes=%zu prior=%u\n",
        unsigned(uint32_t(r.execution.ppu.cpu.gpr[3])), c.spurs_sync_create_calls, int(c.spurs_sync_has_last_failed),
        c.spurs_sync_has_last_failed ? c.spurs_sync_last_failed.native_status : -1, size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY), live_mutexes(r), PRIOR_MUTEXES);
    CHECK(c.spurs_sync_create_calls == 4 && c.spurs_sync_create_retained == 4 && !c.spurs_sync_create_overflow);
    const SyncRecord &m2 = c.spurs_sync_create[2], &k2 = c.spurs_sync_create[3];
    CHECK(m2.operation == StartupSpursSyncOperation::LwmutexCreate && m2.real_call_performed && m2.native_status == PS3_LWMUTEX_OK && m2.mapped_rc == 0);
    CHECK(m2.address == INSTANCE2 + W22_LWMUTEX && snapshot_is(m2.mutexes, table, PRIOR_MUTEXES + 1, table - PRIOR_MUTEXES - 1));
    CHECK(k2.operation == StartupSpursSyncOperation::LwcondCreate && k2.real_call_performed && k2.native_status == PS3_LWCOND_OK && k2.mapped_rc == 0);
    CHECK(k2.address == INSTANCE2 + W23_LWCOND && k2.mutex_address == INSTANCE2 + W22_LWMUTEX && k2.publication_count == 1);
    // 31 prior -> 33 live lwmutexes; 2 conditions, 2 bindings (both tables stay far below their 32 entries); both sync records real
    CHECK(live_mutexes(r) == PRIOR_MUTEXES + 2 && live_conditions(r) == 2 && r.mutexes.condition_binding_count == 2 && sync_pins_active(r) == pins_prior + 4);
    CHECK(bindings_are(k2.mutexes, 1, TARGET_SYNC_TABLE) && snapshot_is(k2.conditions, TARGET_SYNC_TABLE, 1, TARGET_SYNC_TABLE - 1));
    // the first instance is preserved: its synchronization images and SPU group are untouched by the second initialize
    CHECK(snapshot(r, INSTANCE1 + W22_LWMUTEX, 24) == first_mutex && snapshot(r, INSTANCE1 + W23_LWCOND, 8) == first_cond);
    CHECK(r.get8(INSTANCE1 + 0x76) == 5 && r.get8(INSTANCE2 + 0x76) == 1 && r.get32(INSTANCE2 + 0xD30) == 0x04000200u);
    CHECK(r.execution.spurs->instances[0].state == PS3_SPURS_INSTANCE_ACTIVE && r.execution.spurs->instances[1].state == PS3_SPURS_INSTANCE_ACTIVE);
    ps3_spu_group_record g1{}, g2{};
    CHECK(ps3_spu_group_snapshot(&r.policy, 0x04000100u, &g1) == PS3_LV2_OK && g1.state == PS3_SPU_GROUP_RUNNING && g1.num == 5);
    CHECK(ps3_spu_group_snapshot(&r.policy, 0x04000200u, &g2) == PS3_LV2_OK && g2.state == PS3_SPU_GROUP_RUNNING && g2.num == 1);
    CHECK(r.used() == 0x198000 && ps3_ppu_threads_live(&r.comp->ppu) == 4);
    // cleanup releases exactly the SPURS-owned resources; the 31 fixture mutexes (the guest's own) remain
    CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_OK);
    CHECK(r.used() == 0 && ps3_ppu_threads_live(&r.comp->ppu) == 0 && ps3_spurs_runtime_shutdown(&r.comp->runtime) == 0);
    CHECK(live_mutexes(r) == PRIOR_MUTEXES && live_conditions(r) == 0 && r.mutexes.condition_binding_count == 0 && sync_pins_active(r) == pins_prior);
    CHECK(thread_holds_no_tracked_guard());
}
// Service-table and atomic-owner capacity, over the real composed registries after BOTH real SPURS initializes. Both capacities come
// from the shared profile (adapters/startup_capacity_policy.h), derived by hand identically to the header:
//   services = 2 SPURS instances x (helper + handler) + STARTUP_CHILD_SLOT_CAPACITY child records = PS3_PPU_SERVICE_CAP (== 7, 4 + 3);
//   owners   = STARTUP_PRIMARY_OWNER_CAPACITY + STARTUP_SPU_OWNER_CAPACITY (5 + 1 kernel contexts) + STARTUP_CHILD_SLOT_CAPACITY
//              = STARTUP_ATOMIC_OWNER_CAPACITY (== 10, 1 + 6 + 3).
// The fixture reserves one child service record per child slot (real ps3_ppu_service_reserve; nothing is started) and acquires the
// SPURS-held owners plus one child owner per slot (real ps3_atomic_owner_acquire); it never fabricates readiness. Combined
// SPURS+running-children is the root native run.
// Service records live after BOTH initializes: one helper + one handler per SPURS instance.
constexpr size_t SPURS_SERVICE_RECORDS = 2 * size_t(PS3_SPURS_MAX_INSTANCES);
// Atomic owners held after BOTH initializes: the primary owner plus the SPU owner per SPU context (5 + 1 kernel).
constexpr size_t SPURS_HELD_OWNERS = size_t(STARTUP_PRIMARY_OWNER_CAPACITY) + size_t(STARTUP_SPU_OWNER_CAPACITY);
// Refusal fixtures are sized from the same profile: one record beyond the child-slot capacity, and the full owner capacity.
constexpr size_t REFUSAL_SERVICE_THREADS = size_t(STARTUP_CHILD_SLOT_CAPACITY) + 1;
constexpr size_t OWNER_SNAPSHOT_RECORDS = size_t(STARTUP_ATOMIC_OWNER_CAPACITY);
static_assert(int(PS3_PPU_SERVICE_CAP) == int(STARTUP_PPU_SERVICE_CAPACITY),
    "domain service capacity must equal the shared startup capacity profile");
static_assert(SPURS_SERVICE_RECORDS + size_t(STARTUP_CHILD_SLOT_CAPACITY) == size_t(PS3_PPU_SERVICE_CAP),
    "service capacity = (helper + handler) per SPURS instance + one record per child slot");
static_assert(SPURS_HELD_OWNERS + size_t(STARTUP_CHILD_SLOT_CAPACITY) == size_t(STARTUP_ATOMIC_OWNER_CAPACITY),
    "atomic owner capacity = primary + SPU owners + one owner per child slot");
static_assert(OWNER_SNAPSHOT_RECORDS <= 16, "owner snapshot storage covers the shared owner capacity");
size_t owners_active(Runtime &r) {
    const auto guard = tracked_guard_port(&r.g_dom);
    guard.lock(guard.context);
    size_t n = 0;
    for (const auto &owner : r.owners) n += owner.active != 0;
    guard.unlock(guard.context);
    return n;
}
void snapshot_owner_lifetimes(Runtime &r, ps3_atomic_owner_record (&snapshot)[OWNER_SNAPSHOT_RECORDS]) {
    const auto guard = tracked_guard_port(&r.g_dom);
    guard.lock(guard.context);
    for (size_t i = 0; i < OWNER_SNAPSHOT_RECORDS; ++i) {
        snapshot[i].generation = r.owners[i].generation;
        snapshot[i].active = r.owners[i].active;
    }
    guard.unlock(guard.context);
}
void noop_service_body(ps3_ppu_service *self, void *) { ps3_ppu_service_exit(self, 0); }
void run_target_profile_service_and_owner_capacity() {
    Runtime r(1, true, true, WINDOW, WINDOW_END, QUOTA, 2, 4, true);
    r.spu_initialize(6, 0); r.attribute(5, 128, 249); r.call_initialize(INSTANCE1);
    r.attribute(1, 128, 249); r.call_initialize(INSTANCE2);
    auto &c = *r.control;
    CHECK(c.stop == StartupStop::None && c.spurs_initialize_returns == 2 && r.execution.ppu.cpu.gpr[3] == 0);
    ps3_ppu_threads &ppu = r.comp->ppu;
    CHECK(ps3_ppu_threads_live(&ppu) == 4 && ps3_ppu_threads_main_id(&ppu) == 1);
    // ---- service capacity: every child slot's record coexists with the four SPURS records; the next one is refused without effects
    constexpr unsigned slots = unsigned(STARTUP_CHILD_SLOT_CAPACITY);
    ps3_pthread_service_thread extra[REFUSAL_SERVICE_THREADS]{};
    ps3_ppu_ref child[slots]{}, refused{0xDEADu, 0xBEEFu};
    const uint64_t used_services = r.used();
    for (unsigned i = 0; i < slots; ++i) {
        CHECK(ps3_ppu_service_reserve(&ppu, 1, 249, 250, 0x1000, ps3_pthread_service_thread_port(&extra[i]), noop_service_body, nullptr, &child[i]) == PS3_PPU_OK);
        CHECK(ps3_ppu_threads_live(&ppu) == SPURS_SERVICE_RECORDS + i + 1 && r.used() == used_services + 0x1000 * (i + 1));
    }
    for (unsigned i = 0; i < slots; ++i) for (unsigned j = 0; j < slots; ++j) if (i != j) CHECK(child[i].id != child[j].id);
    CHECK(ps3_ppu_threads_live(&ppu) == size_t(PS3_PPU_SERVICE_CAP));
    CHECK(ps3_ppu_service_reserve(&ppu, 1, 249, 250, 0x1000, ps3_pthread_service_thread_port(&extra[slots]), noop_service_body, nullptr, &refused) == PS3_PPU_EAGAIN);
    CHECK(refused.slot == 0xDEADu && refused.id == 0xBEEFu && ps3_ppu_threads_live(&ppu) == size_t(PS3_PPU_SERVICE_CAP) &&
          r.used() == used_services + 0x1000u * slots && !extra[slots].started);
    for (unsigned i = 0; i < slots; ++i) CHECK(ps3_ppu_service_abandon(&ppu, child[i]) == PS3_PPU_OK);   // releases their own quota
    CHECK(ps3_ppu_threads_live(&ppu) == SPURS_SERVICE_RECORDS && r.used() == used_services);
    // ---- atomic owner capacity: primary + five SPU contexts of instance 1 + one of instance 2 = 7 held; one child owner per slot fills it
    CHECK(owners_active(r) == SPURS_HELD_OWNERS);
    ps3_atomic_owner child_owner[slots]{}, overflow{};
    for (unsigned i = 0; i < slots; ++i) {
        CHECK(ps3_atomic_owner_acquire(&r.domain, &child_owner[i]) == PS3_GUEST_MEMORY_OK);
        CHECK(owners_active(r) == SPURS_HELD_OWNERS + i + 1);
    }
    for (unsigned i = 0; i < slots; ++i) for (unsigned j = 0; j < slots; ++j) if (i != j) CHECK(child_owner[i].slot != child_owner[j].slot);
    CHECK(owners_active(r) == size_t(STARTUP_ATOMIC_OWNER_CAPACITY));
    static_assert(size_t(STARTUP_ATOMIC_OWNER_CAPACITY) <= 16, "owner snapshot storage covers the shared owner capacity");
    ps3_atomic_owner_record owners_before_refusal[OWNER_SNAPSHOT_RECORDS]{}, owners_after_refusal[OWNER_SNAPSHOT_RECORDS]{};
    snapshot_owner_lifetimes(r, owners_before_refusal);
    const size_t pins_full = sync_pins_active(r);
    CHECK(ps3_atomic_owner_acquire(&r.domain, &overflow) == PS3_GUEST_MEMORY_NO_MEMORY);   // one distinct owner beyond the capacity must fail with no effect
    CHECK(owners_active(r) == size_t(STARTUP_ATOMIC_OWNER_CAPACITY) && sync_pins_active(r) == pins_full && overflow.domain == nullptr);
    snapshot_owner_lifetimes(r, owners_after_refusal);
    for (size_t i = 0; i < OWNER_SNAPSHOT_RECORDS; ++i) {
        CHECK(owners_before_refusal[i].generation == owners_after_refusal[i].generation);
        CHECK(owners_before_refusal[i].active == owners_after_refusal[i].active);
    }
    for (unsigned i = 0; i < slots; ++i) CHECK(ps3_atomic_owner_release(&r.domain, &child_owner[i]) == PS3_GUEST_MEMORY_OK);
    CHECK(owners_active(r) == SPURS_HELD_OWNERS);
    ps3_atomic_owner again{};
    CHECK(ps3_atomic_owner_acquire(&r.domain, &again) == PS3_GUEST_MEMORY_OK && ps3_atomic_owner_release(&r.domain, &again) == PS3_GUEST_MEMORY_OK);
    CHECK(native_startup_release_after_join(r.execution) == PS3_GUEST_MEMORY_OK);
    std::printf("SPURS_INITIALIZE_BRIDGE_TARGET after_release used=0x%llx live_services=%zu owners_active=%zu\n", (unsigned long long)r.used(), ps3_ppu_threads_live(&ppu), owners_active(r));
    CHECK(r.used() == 0 && ps3_ppu_threads_live(&ppu) == 0 && owners_active(r) == 0);   // shutdown releases the SPU owners and the primary (PPU admission) owner: every owner is free again
}
int main() {
    std::printf("SPURS_INITIALIZE_BRIDGE_LAYOUT startup_owner=%zu control=%zu composition=%zu runtime=%zu\n",
        sizeof(StartupExecution), sizeof(StartupControl), sizeof(StartupSpursComposition), sizeof(ps3_spurs_runtime));
    run_one(1, 0x01000000u, 0x01000001u);            // the real startup main thread id (1): services start at the base
    run_one(0x01000000u, 0x01000001u, 0x01000002u);  // L5 collision: main id sits on the first service id; services skip it
    run_sync_trace_lanes();
    run_prior_allocation_quota();
    run_quota_report_calculation();
    run_negatives();
    run_sync_create_both_available();
    run_sync_create_mutex_objects_full();
    run_sync_create_condition_binding_credits_full();
    run_sync_create_condition_objects_full();
    run_sync_create_provider_errors_are_not_resource_exhaustion();
    run_sync_create_overflow_is_bounded_and_inert();
    run_sync_create_through_initialize();
    run_guest_event_queue_after_spurs();
    run_target_profile_second_spurs_after_31_prior_mutexes();
    run_target_profile_service_and_owner_capacity();
    std::printf("SPURS_INITIALIZE_BRIDGE_HOST failures=%d native_runtime=false game_boot=false\n", failures);
    return failures != 0;
}

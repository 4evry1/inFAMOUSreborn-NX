/* Host behavior test for B9BC6207 (cellSpursAttachLv2EventQueue) through the REAL dispatcher (ps3_hle_call / lv2_syscall) and the
 * REAL composition (startup_spurs_compose) over real collaborators: atomic domain + coherent guest memory, lv2 objects, SPU group
 * policy (configured only through the guest's own syscall 169), PPU service registry with real host threads, the kernel-1 idle
 * service, the real lwmutex/lwcond registries and the new ps3_spurs_runtime_attach_lv2_event_queue domain. The generated call
 * site is modelled by the call fixture (argument registers exactly as the import stub sets them); this is a label fixture, not
 * the game, and no test here claims game boot or render. Expected values are hand-derived from the contract, never captured. */
#include "../adapters/startup_execution.cpp"
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
#include <vector>
#include <pthread.h>
#include <initializer_list>
#include <type_traits>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("SPURS_ATTACH_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
std::atomic<uint64_t> tick_calls{0};   // armGetSystemTick: a deterministic poll counter (deadline sweep); only the PPU thread polls
constexpr uint32_t INSTANCE1 = 0x7CF100, INSTANCE2 = 0x7D0380, ATTR = 0xD00FF990, STACK = 0xD00FF910, TOC_SLOT = STACK + 0x28;
constexpr uint32_t STACK_BASE = 0xD00FF000, STACK_SPAN = 0x3000, HOST_STACK = 0x800000, HOST_SIZE = 0x900000;
constexpr uint32_t PORT_EA = STACK_BASE + 0x500, QUEUE_ATTR = STACK_BASE + 0x400, QUEUE_OUT = STACK_BASE + 0x440;
constexpr uint32_t OFF_EXCEPTION = 0xD6C, OFF_SDK = 0xDA4, OFF_BITS = 0xDA8;
constexpr uint64_t TOC = UINT64_C(0x123456789abcdef0);
constexpr uint32_t WINDOW = 0x400000, WINDOW_END = 0x700000, QUOTA = 0x300000, ATTACH_NID = 0xb9bc6207, SDK = 0x250001;
constexpr uint32_t INTERNAL_QUEUE = 0x8D000000u, ABSENT_QUEUE = 0x8D00FF00u;
static_assert(STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS == 64, "documented caller policy");
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
#define GS(NAME, ...) static ps3_guest_memory_status NAME(void *p, uint32_t a, __VA_ARGS__) { uint32_t h; auto *g = static_cast<GuestSpace *>(p); \
        return map(a, n, h) ? g->backing.NAME(g->backing.context, h, CALL_ARGS) : PS3_GUEST_MEMORY_INVALID_RANGE; }
#define CALL_ARGS n
    GS(commit, uint64_t n) GS(decommit, uint64_t n) GS(zero, uint64_t n)
#undef CALL_ARGS
#define CALL_ARGS b, n
    GS(read, void *b, size_t n)
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *b, size_t n) { uint32_t h; auto *g = static_cast<GuestSpace *>(p);
        return map(a, n, h) ? g->backing.write(g->backing.context, h, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE; }
#undef CALL_ARGS
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};
struct ConditionWait {
    pthread_cond_t condition{};
    ConditionWait() { pthread_condattr_t attr;
        CHECK(!pthread_condattr_init(&attr) && !pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) && !pthread_cond_init(&condition, &attr) && !pthread_condattr_destroy(&attr)); }
    ~ConditionWait() { CHECK(!pthread_cond_destroy(&condition)); }
    static ps3_wait_status now(void *, uint64_t *output) { timespec v{}; if (clock_gettime(CLOCK_MONOTONIC, &v)) std::abort();
        *output = uint64_t(v.tv_sec) * UINT64_C(1000000000) + uint64_t(v.tv_nsec); return PS3_WAIT_READY; }
    static ps3_wait_status park(void *context, const ps3_instance_guard *guard, uint64_t deadline, ps3_wait_cancel cancel) {
        if (cancel.is_cancelled && cancel.is_cancelled(cancel.context)) return PS3_WAIT_CANCELLED;
        uint64_t v = 0; now(context, &v);
        const uint64_t t = deadline < v + 2000000 ? deadline : v + 2000000; const timespec until{time_t(t / 1000000000), long(t % 1000000000)};
        const int s = pthread_cond_timedwait(&static_cast<ConditionWait *>(context)->condition, static_cast<pthread_mutex_t *>(guard->context), &until);
        if (s && s != ETIMEDOUT) std::abort();
        return PS3_WAIT_READY; }
    static void notify(void *context) { if (pthread_cond_broadcast(&static_cast<ConditionWait *>(context)->condition)) std::abort(); }
    ps3_wait_port port() { return {this, now, park, notify}; }
};
ps3_lwcond_thread_status main_exists(void *context, uint32_t id, unsigned *output) { *output = id == *static_cast<uint32_t *>(context); return PS3_LWCOND_THREAD_OK; }
struct Runtime {
    GuestSpace guest;
    ps3_pthread_guard g_dom, g_mem, g_lv2, g_ppu, g_pol, g_mutex, g_cond;
    ps3_pthread_wait_port port;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[16]{};
    ps3_atomic_pin_record pins[128]{};
    ps3_user_memory memory{};
    ps3_user_memory_slot slots[4]{};
    ps3_spu_resource_policy policy{};
    ConditionWait mutex_wait, cond_wait;
    ps3_lwmutex_registry mutexes{};
    ps3_lwmutex_object mutex_objects[4]{};
    ps3_lwmutex_waiter mutex_waiters[8]{};
    ps3_lwcond_registry conditions{};
    ps3_lwcond_object condition_objects[4]{};
    ps3_lwcond_waiter condition_waiters[4]{};
    uint32_t main_id = 1;
    PpuMemoryRamRange ranges[2]{{0x7CF000, 0x3000, true}, {STACK_BASE, STACK_SPAN, true}};
    std::unique_ptr<PpuMemoryBinding> binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(4096);
    PpuMemoryTraceRecorder recorder;
    static ps3_clock_status sample(void *, uint64_t *value) { *value = 1; return PS3_CLOCK_OK; }
    ps3_clock clock{nullptr, sample};
    infamous::discovery::DiscoveryBudget budget_policy{{64, 64, 64, UINT64_C(1000000000)}, 0};
    DiscoveryBudgetBinding budget{budget_policy, clock};
    std::unique_ptr<StartupSpursComposition> comp{new StartupSpursComposition};
    ps3_pthread_service_thread threads[PS3_SPURS_MAX_INSTANCES][3]{};
    std::vector<unsigned char> local_stores = std::vector<unsigned char>(size_t(PS3_SPURS_LS_SLOTS) * PS3_SPURS_K1_LS_BYTES);
    StartupSpursPorts ports;
    bool composed = false;
    Runtime() {
        CHECK(!ps3_pthread_guard_init(&g_dom) && !ps3_pthread_guard_init(&g_mem) && !ps3_pthread_guard_init(&g_lv2) && !ps3_pthread_guard_init(&g_ppu) &&
              !ps3_pthread_guard_init(&g_pol) && !ps3_pthread_guard_init(&g_mutex) && !ps3_pthread_guard_init(&g_cond) && !ps3_pthread_wait_port_init(&port));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), ps3_pthread_guard_port(&g_dom), owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        const ps3_user_memory_config mc{WINDOW, WINDOW_END, QUOTA, ps3_atomic_domain_memory(&domain), ps3_pthread_guard_port(&g_mem)};
        CHECK(ps3_user_memory_init(&memory, &mc, slots, 2) == PS3_USER_MEMORY_OK);
        CHECK(ps3_spu_resource_policy_init(&policy, ps3_pthread_guard_port(&g_pol)) == PS3_SPU_POLICY_OK);
        CHECK(ps3_lwmutex_init_with_conditions(&mutexes, &domain, ps3_pthread_guard_port(&g_mutex), mutex_wait.port(), mutex_objects, 4, mutex_waiters, 8, 4).status == PS3_LWMUTEX_OK);
        CHECK(ps3_lwcond_init(&conditions, &mutexes, ps3_pthread_guard_port(&g_cond), cond_wait.port(), {&main_id, main_exists}, condition_objects, 4, condition_waiters, 4).status == PS3_LWCOND_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges, 2));
        CHECK(ps3_atomic_commit(&domain, 0x7CF000, 0x3000) == PS3_GUEST_MEMORY_OK && ps3_atomic_commit(&domain, STACK_BASE, STACK_SPAN) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding->coherent_memory;
        execution.control = control.get(); execution.conditions = &conditions; execution.spu_resources = &policy;
        execution.guest_priority = 250; execution.has_guest_priority = true;
        execution.ppu.cpu.thread_id = 1; execution.ppu.cpu.gpr[1] = STACK; execution.ppu.cpu.gpr[2] = TOC; execution.ppu.cpu.lr = 0x2cfe70;
        execution.process_sdk = {SDK, true};   // explicit effective process configuration (as the composition root derives it from the plan)
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Strict, 0));
        CHECK(native_startup_admit_budgeted(execution, *binding, services, recorder, budget));
        for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) ports.local_stores[i] = &local_stores[size_t(i) * PS3_SPURS_K1_LS_BYTES];
        ports.notify = ps3_pthread_wait_port_port(&port); ports.lv2_guard = ps3_pthread_guard_port(&g_lv2); ports.ppu_guard = ps3_pthread_guard_port(&g_ppu);
        for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) for (unsigned r = 0; r < 3; ++r) ports.threads[i][r] = ps3_pthread_service_thread_port(&threads[i][r]);
        composed = startup_spurs_compose(execution, *comp, domain, memory, policy, ports) == StartupSpursComposeStatus::Ok;
        CHECK(composed && execution.spurs == &comp->runtime);
    }
    ~Runtime() {   // child join is moot here; SPURS shutdown (exact detach) must come BEFORE the guest queue/port release
        CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        binding.reset();
        CHECK(ps3_user_memory_cleanup(&memory).status == PS3_USER_MEMORY_OK);
        ps3_lwcond_report report{}; CHECK(ps3_lwcond_cleanup(&conditions, &report).status == PS3_LWCOND_OK); CHECK(ps3_lwmutex_cleanup(&mutexes).status == PS3_LWMUTEX_OK);
        ps3_pthread_wait_port_destroy(&port);
        ps3_pthread_guard_destroy(&g_dom); ps3_pthread_guard_destroy(&g_mem); ps3_pthread_guard_destroy(&g_lv2); ps3_pthread_guard_destroy(&g_ppu);
        ps3_pthread_guard_destroy(&g_pol); ps3_pthread_guard_destroy(&g_mutex); ps3_pthread_guard_destroy(&g_cond);
    }
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n) { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    void put8(uint32_t a, uint8_t v) { put(a, &v, 1); }
    void put32(uint32_t a, uint32_t v) { const unsigned char e[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; put(a, e, 4); }
    uint64_t get(uint32_t a, size_t n) { unsigned char b[8]{}; CHECK(mem().read(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); uint64_t v = 0; for (size_t i = 0; i < n; ++i) v = v << 8 | b[i]; return v; }
    uint64_t bits() { return get(INSTANCE1 + OFF_BITS, 8); }
    ps3_spurs_instance &instance() { return comp->runtime.instances[0]; }
    void put_attribute() { unsigned char image[512]{}; const uint32_t w[5]{2, SDK, 5, 128, 249};
        for (unsigned i = 0; i < 5; ++i) for (unsigned b = 0; b < 4; ++b) image[4 * i + b] = uint8_t(w[i] >> (24 - 8 * b));
        put(ATTR, image, sizeof image); }
    void run(uint32_t nid_or_sc, bool syscall) {
        auto &cpu = execution.ppu.cpu; control->stop = StartupStop::None; active = control.get(); control->cpu = &cpu;
        try { if (syscall) { cpu.gpr[11] = nid_or_sc; lv2_syscall(&cpu); } else ps3_hle_call(nid_or_sc, &cpu); active = nullptr; } catch (const Terminal &) {}
    }
    void boot() {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[3] = 6; cpu.gpr[4] = 0; run(169, true); CHECK(control->stop == StartupStop::None && cpu.gpr[3] == 0);
        put_attribute(); cpu.gpr[3] = INSTANCE1; cpu.gpr[4] = ATTR; cpu.gpr[5] = cpu.gpr[6] = cpu.gpr[7] = cpu.gpr[8] = 0; cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        run(0xaa6269a8, false); CHECK(control->stop == StartupStop::None && cpu.gpr[3] == 0 && instance().state == PS3_SPURS_INSTANCE_ACTIVE);
        put8(PORT_EA, 1);   // the real game's stack byte: static port 1
    }
    uint32_t make_queue() {   // guest sc128 over the registry SPURS owns: PRIORITY, PPU
        const unsigned char attr[16]{0, 0, 0, 2, 0, 0, 0, 1, 0, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E};
        put(QUEUE_ATTR, attr, sizeof attr); put32(QUEUE_OUT, 0);
        auto &cpu = execution.ppu.cpu; cpu.gpr[3] = QUEUE_OUT; cpu.gpr[4] = QUEUE_ATTR; cpu.gpr[5] = 0; cpu.gpr[6] = 127; run(128, true);
        CHECK(control->stop == StartupStop::None && cpu.gpr[3] == 0);
        return uint32_t(get(QUEUE_OUT, 4));
    }
    void attach(uint64_t spurs, uint64_t queue, uint64_t port_ea, uint64_t dynamic, uint64_t sp = STACK) {
        auto &cpu = execution.ppu.cpu; cpu.gpr[3] = spurs; cpu.gpr[4] = queue; cpu.gpr[5] = port_ea; cpu.gpr[6] = dynamic; cpu.gpr[7] = cpu.gpr[8] = 0;
        cpu.gpr[1] = sp; cpu.gpr[2] = TOC;
        control->spurs_attach_reads = control->spurs_attach_writes = control->spurs_attach_cas_calls = control->spurs_attach_cas_committed = 0;   // per-call counts
        run(ATTACH_NID, false);
    }
    const ps3_spu_group_record *group() { for (const auto &g : policy.groups) if (g.active && g.id == instance().group) return &g; return nullptr; }
    unsigned bound_threads(uint8_t spup, uint32_t queue) { unsigned n = 0; const auto *g = group(); CHECK(g); if (g) for (uint32_t t = 0; t < g->num; ++t) n += g->threads[t].port_queue[spup] == queue; return n; }
    // Events the call published (everything after `from`), in order.
    std::vector<PpuMemoryTraceEvent> since(size_t from) { std::vector<PpuMemoryTraceEvent> out; PpuMemoryTraceSample s; for (size_t i = from; recorder.read(i, s); ++i) out.push_back(s.event); return out; }
    size_t published() { return recorder.report().retained_count; }
    void leave_trace_capacity(size_t leave) {   // consume real recorder credit until exactly `leave` publications can still begin
        while (recorder.can_begin(leave + 1)) { CHECK(recorder.begin(1)); recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 0x7CF000, 1, 0)); recorder.finish(); }
        CHECK((leave == 0 || recorder.can_begin(leave)) && !recorder.can_begin(leave + 1));
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return tick_calls.fetch_add(1); }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }
// Test-only link wrapper (this binary only: -Wl,--wrap=ps3_atomic_compare_exchange64 in verify_startup_spurs_attach.sh; no production hook).
// Inert unless armed: every other lane calls the REAL CAS untouched. When armed, on the FIRST attempt at the target address it performs a
// concurrent ps3_atomic_write (a real, different DA8 bit, as a SPURS kernel worker would) and only then invokes the REAL compare-exchange,
// which therefore fails with the real observed value; later attempts are untouched (and succeed).
extern "C" ps3_guest_memory_status __real_ps3_atomic_compare_exchange64(ps3_atomic_domain *, uint64_t, uint64_t, uint64_t, ps3_atomic_compare_result *);
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_compare_exchange64(ps3_atomic_domain *, uint64_t, uint64_t, uint64_t, ps3_atomic_compare_result *);
static_assert(std::is_same<decltype(&__wrap_ps3_atomic_compare_exchange64), decltype(&ps3_atomic_compare_exchange64)>::value &&
              std::is_same<decltype(&__real_ps3_atomic_compare_exchange64), decltype(&ps3_atomic_compare_exchange64)>::value,
              "the wrap must have the exact signature of the wrapped function");
struct CasContention {
    bool armed = false, injected = false, write_ok = false;
    uint32_t address = 0;
    uint64_t or_bits = 0;
    unsigned calls = 0;   // every wrapped call at the target address while armed
};
CasContention cas_contention;
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_compare_exchange64(ps3_atomic_domain *domain, uint64_t address, uint64_t expected, uint64_t desired,
    ps3_atomic_compare_result *result)
{
    if (cas_contention.armed && address == cas_contention.address) {
        ++cas_contention.calls;
        if (!cas_contention.injected) {
            cas_contention.injected = true;
            unsigned char bytes[8]{};
            uint64_t value = 0;
            if (ps3_atomic_read(domain, address, bytes, 8) == PS3_GUEST_MEMORY_OK) {
                for (unsigned i = 0; i < 8; ++i) value = value << 8 | bytes[i];
                value |= cas_contention.or_bits;
                for (unsigned i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (56 - 8 * i));
                cas_contention.write_ok = ps3_atomic_write(domain, address, bytes, 8) == PS3_GUEST_MEMORY_OK;
            }
        }
    }
    return __real_ps3_atomic_compare_exchange64(domain, address, expected, desired, result);
}
namespace {
uint64_t rc_reg(uint32_t rc) { return uint64_t(int64_t(int32_t(rc))); }
bool is_event(const PpuMemoryTraceEvent &e, bool write, uint32_t address, size_t length, uint64_t value) {
    return e.origin == PpuMemoryTraceOrigin::Hle && (e.operation == PpuMemoryTraceOperation::Write) == write && e.address == address && e.length == length && e.value == value; }
// Nothing observable changed: the DA8 word, the port byte, every binding and the ledger; the instance is still ACTIVE.
struct Quiet { uint64_t bits; uint8_t port; ps3_lv2_queue q; };
Quiet quiet_of(Runtime &r, uint32_t queue) { Quiet q{r.bits(), uint8_t(r.get(PORT_EA, 1)), {}}; ps3_lv2_queue_snapshot(&r.comp->lv2, queue, &q.q); return q; }
bool unchanged(Runtime &r, const Quiet &before, uint32_t queue) {
    const Quiet after = quiet_of(r, queue);
    return after.bits == before.bits && after.port == before.port && !std::memcmp(&after.q, &before.q, sizeof after.q) && r.instance().user_port_bits == 0; }

void run_happy_and_exact_trace() {
    Runtime r; r.boot(); const uint32_t q = r.make_queue();
    CHECK(q == INTERNAL_QUEUE + 0x100u * r.comp->lv2.serial[PS3_LV2_KIND_QUEUE] - 0x100u);   // derived from the lv2 id contract
    unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8);
    const Quiet before = quiet_of(r, q); ps3_lv2_queue helper_before{}; CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, INTERNAL_QUEUE, &helper_before) == PS3_LV2_OK);
    const uint64_t prior = r.bits(); const size_t from = r.published();
    r.attach(INSTANCE1, UINT64_C(0xDEADBEEF00000000) | q, PORT_EA, UINT64_C(0xFFFFFFFF00000000));   // 32-bit ABI: high halves are ignored
    auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && cpu.gpr[2] == TOC && cpu.gpr[1] == STACK);
    CHECK(c.spurs_attach_calls == 1 && c.spurs_attach_returns == 1 && c.spurs_attach_has_result && c.spurs_attach_has_sdk && c.spurs_attach_sdk == SDK);
    CHECK(c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_OK && c.spurs_attach_result.rc == 0 && c.spurs_attach_result.step == PS3_SPURS_ATTACH_STEP_DONE);
    CHECK(!c.spurs_attach_callback_error && !c.spurs_attach_trace_failure && c.spurs_attach_stop_latched == StartupStop::None);
    CHECK(c.spurs_attach_reads == 3 && c.spurs_attach_writes == 1 && c.spurs_attach_cas_calls == 1 && c.spurs_attach_cas_committed == 1);
    CHECK(c.spurs_attach_info.spup == 1 && c.spurs_attach_info.port_written && c.spurs_attach_info.cas_attempts == 1 && c.spurs_attach_info.commit_status == 0);
    CHECK(c.spurs_attach_arguments[0] == INSTANCE1 && uint32_t(c.spurs_attach_arguments[1]) == q && c.spurs_attach_arguments[2] == PORT_EA);
    CHECK(r.get(PORT_EA, 1) == 1 && r.bits() == (prior | 2) && r.get(TOC_SLOT, 8) == TOC);   // port 1 -> DA8 bit 1 (mask 2), prior bits preserved
    CHECK(r.bound_threads(1, q) == 5 && r.instance().user_port_bits == 2 && r.instance().user_port_queue[1] == q && r.instance().state == PS3_SPURS_INSTANCE_ACTIVE);
    const auto ev = r.since(from);   // exception read, port read, DA8 seed read, port write, DA8 CAS write, TOC write: all through the real recorder
    CHECK(ev.size() == 6 && is_event(ev[0], false, INSTANCE1 + OFF_EXCEPTION, 4, 0) && is_event(ev[1], false, PORT_EA, 1, 1) && is_event(ev[2], false, INSTANCE1 + OFF_BITS, 8, prior) &&
          is_event(ev[3], true, PORT_EA, 1, 1) && is_event(ev[4], true, INSTANCE1 + OFF_BITS, 8, prior | 2) && is_event(ev[5], true, TOC_SLOT, 8, TOC));
    ps3_lv2_queue helper_after{}; CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, INTERNAL_QUEUE, &helper_after) == PS3_LV2_OK && !std::memcmp(&helper_before, &helper_after, sizeof helper_after));
    ps3_lv2_queue guest{}; CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, q, &guest) == PS3_LV2_OK && !std::memcmp(&guest, &before.q, sizeof guest));   // no event sent, no wake, not drained
    r.attach(INSTANCE1, q, PORT_EA, 0);   // same queue/port again: guest-visible BUSY (EISCONN), no further effects
    CHECK(c.stop == StartupStop::None && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_FAILED && c.spurs_attach_result.rc == PS3_SPURS_RC_BUSY && cpu.gpr[3] == rc_reg(PS3_SPURS_RC_BUSY));
    CHECK(r.bits() == (prior | 2) && r.bound_threads(1, q) == 5 && r.instance().user_port_bits == 2);
}
// Review M1: a real concurrent DA8 write lands between the domain's seed read and its CAS, so the first REAL CAS fails (OK, exchanged==0) and the
// domain retries from the observed value. The failed CAS must publish the Read of the value it actually observed (BE semantic value), then the
// retry publishes the committed Write; no extra credit is spent without an event and no queue event is invented.
void run_cas_contention() {
    Runtime r; r.boot(); const uint32_t q = r.make_queue();
    const uint64_t prior = r.bits();
    uint64_t concurrent = 4; while ((prior | 2) & concurrent) concurrent <<= 1;   // a DA8 bit that is neither a prior bit nor the port-1 mask (2)
    const Quiet before = quiet_of(r, q); ps3_lv2_queue helper_before{}; CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, INTERNAL_QUEUE, &helper_before) == PS3_LV2_OK);
    const size_t from = r.published();
    cas_contention = {}; cas_contention.armed = true; cas_contention.address = INSTANCE1 + OFF_BITS; cas_contention.or_bits = concurrent;
    r.attach(INSTANCE1, q, PORT_EA, 0);
    const CasContention seen = cas_contention; cas_contention = {};   // disarm before any further check or the Runtime destructor
    auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    CHECK(seen.injected && seen.write_ok && seen.calls == 2);   // first attempt contended through the real atomic write, second attempt untouched
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && cpu.gpr[2] == TOC && cpu.gpr[1] == STACK);
    CHECK(c.spurs_attach_has_result && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_OK && c.spurs_attach_result.rc == 0 && c.spurs_attach_result.step == PS3_SPURS_ATTACH_STEP_DONE);
    CHECK(!c.spurs_attach_callback_error && !c.spurs_attach_trace_failure && c.spurs_attach_stop_latched == StartupStop::None);
    CHECK(c.spurs_attach_info.cas_attempts == 2 && c.spurs_attach_info.commit_status == 0 && c.spurs_attach_info.port_written && c.spurs_attach_info.spup == 1);
    CHECK(c.spurs_attach_reads == 3 && c.spurs_attach_writes == 1 && c.spurs_attach_cas_calls == 2 && c.spurs_attach_cas_committed == 1);
    CHECK(r.bits() == (prior | concurrent | 2) && (r.bits() & prior) == prior && (r.bits() & concurrent) == concurrent);   // prior + concurrent + port bits all kept
    CHECK(r.get(PORT_EA, 1) == 1 && r.get(TOC_SLOT, 8) == TOC && r.bound_threads(1, q) == 5 && r.instance().user_port_bits == 2 && r.instance().user_port_queue[1] == q);
    CHECK(r.instance().state == PS3_SPURS_INSTANCE_ACTIVE);
    const auto ev = r.since(from);   // exception read, port read, DA8 seed read, port write, FAILED-CAS read (observed), committed CAS write, TOC write
    CHECK(ev.size() == 7);
    if (ev.size() == 7) {
        CHECK(is_event(ev[0], false, INSTANCE1 + OFF_EXCEPTION, 4, 0) && is_event(ev[1], false, PORT_EA, 1, 1) && is_event(ev[2], false, INSTANCE1 + OFF_BITS, 8, prior));
        CHECK(is_event(ev[3], true, PORT_EA, 1, 1));
        CHECK(is_event(ev[4], false, INSTANCE1 + OFF_BITS, 8, prior | concurrent));       // the value the failed CAS actually observed
        CHECK(is_event(ev[5], true, INSTANCE1 + OFF_BITS, 8, prior | concurrent | 2));    // the retry's committed write of desired
        CHECK(is_event(ev[6], true, TOC_SLOT, 8, TOC));
    }
    ps3_lv2_queue helper_after{}; CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, INTERNAL_QUEUE, &helper_after) == PS3_LV2_OK && !std::memcmp(&helper_before, &helper_after, sizeof helper_after));
    ps3_lv2_queue guest{}; CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, q, &guest) == PS3_LV2_OK && !std::memcmp(&guest, &before.q, sizeof guest));   // borrowed queue: no event, no wake
    CHECK(!cas_contention.armed && !cas_contention.injected);   // exact cleanup: wrapper inert again; the Runtime destructor requires release OK
}
void run_sdk_policy() {
    uint32_t e = 0;
    CHECK(startup_spurs_attach_effective_sdk({SDK, true}, &e) && e == SDK && startup_spurs_attach_effective_sdk({0xFFFFFFFFu, true}, &e) && e == STARTUP_SPURS_ATTACH_MAPPED_UNSET_SDK);
    CHECK(!startup_spurs_attach_effective_sdk({0x80000000u, true}, &e) && !startup_spurs_attach_effective_sdk({0xFFFFFFFEu, true}, &e) && !startup_spurs_attach_effective_sdk({SDK, false}, &e) && !startup_spurs_attach_effective_sdk({SDK, true}, nullptr));
    {   // tampered DA4 cannot change the policy: process SDK 0x250001, DA4 claims a pre-0x180000 SDK, port 16 still PERM
        Runtime r; r.boot(); const uint32_t q = r.make_queue(); r.put32(INSTANCE1 + OFF_SDK, 0x100000); r.put8(PORT_EA, 16);
        const Quiet before = quiet_of(r, q); r.attach(INSTANCE1, q, PORT_EA, 0);
        CHECK(r.control->stop == StartupStop::None && r.control->spurs_attach_result.outcome == PS3_SPURS_OUTCOME_FAILED && r.control->spurs_attach_result.rc == PS3_SPURS_RC_PERM);
        CHECK(r.execution.ppu.cpu.gpr[3] == rc_reg(PS3_SPURS_RC_PERM) && unchanged(r, before, q) && r.control->spurs_attach_writes == 0);
    }
    {   // and the opposite: process SDK older than 0x180000 with DA4 claiming 0x250001: port 16 is the domain's old-SDK STOP; port 15 binds
        Runtime r; r.execution.process_sdk = {0x17FFFF, true}; r.boot(); const uint32_t q = r.make_queue(); r.put32(INSTANCE1 + OFF_SDK, SDK); r.put8(PORT_EA, 16);
        const Quiet before = quiet_of(r, q); r.attach(INSTANCE1, q, PORT_EA, 0);
        CHECK(r.control->stop == StartupStop::SpursAttachStop && r.control->spurs_attach_sdk == 0x17FFFF && r.control->spurs_attach_result.reason == PS3_SPURS_STOP_INVALID_REQUEST &&
              r.control->spurs_attach_result.step == PS3_SPURS_ATTACH_STEP_PORT_RANGE && unchanged(r, before, q) && r.instance().state == PS3_SPURS_INSTANCE_ACTIVE);
    }
    {   // raw -1 is faithfully mapped to 0x485000 (loader rejects it today); a bind of port 1 works under the mapped value
        Runtime r; r.execution.process_sdk = {0xFFFFFFFFu, true}; r.boot(); const uint32_t q = r.make_queue(); r.attach(INSTANCE1, q, PORT_EA, 0);
        CHECK(r.control->stop == StartupStop::None && r.control->spurs_attach_sdk == 0x485000 && r.execution.ppu.cpu.gpr[3] == 0 && r.bound_threads(1, q) == 5);
    }
    for (const StartupProcessSdk bad : {StartupProcessSdk{0x80000000u, true}, StartupProcessSdk{0xFFFFFFFEu, true}, StartupProcessSdk{SDK, false}}) {   // fail closed BEFORE any effect
        Runtime r; r.boot(); const uint32_t q = r.make_queue(); r.execution.process_sdk = bad;
        unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8); const Quiet before = quiet_of(r, q); const size_t from = r.published();
        r.attach(INSTANCE1, q, PORT_EA, 0);
        CHECK(r.control->stop == (bad.valid ? StartupStop::SpursAttachStop : StartupStop::MissingServices));
        CHECK(!r.control->spurs_attach_has_result && r.control->spurs_attach_reads == 0 && r.control->spurs_attach_writes == 0 && unchanged(r, before, q) && r.since(from).empty() && r.get(TOC_SLOT, 1) == 0x5A);
    }
}

void run_validation() {
    Runtime r; r.boot(); const uint32_t q = r.make_queue(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const Quiet before = quiet_of(r, q);
    auto failed = [&](uint32_t rc, uint32_t reads, const char *what) {   // guest-visible FAILED: r3 = rc, TOC published, no effect of any kind
        const bool ok = c.stop == StartupStop::None && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_FAILED && c.spurs_attach_result.rc == rc &&
            cpu.gpr[3] == rc_reg(rc) && c.spurs_attach_reads == reads && c.spurs_attach_writes == 0 && c.spurs_attach_cas_calls == 0 && unchanged(r, before, q);
        if (!ok) std::printf("validation lane failed: %s\n", what);
        CHECK(ok);
    };
    auto stopped = [&](StartupStop stop, uint32_t reason, uint32_t step, uint32_t reads, const char *what) {   // pre-effect STOP: instance untouched
        const bool ok = c.stop == stop && c.spurs_attach_has_result && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_STOP && c.spurs_attach_result.reason == reason &&
            c.spurs_attach_result.step == step && c.spurs_attach_reads == reads && c.spurs_attach_writes == 0 && c.spurs_attach_cas_calls == 0 && unchanged(r, before, q) &&
            r.instance().state == PS3_SPURS_INSTANCE_ACTIVE;
        if (!ok) std::printf("validation lane failed: %s\n", what);
        CHECK(ok);
    };
    r.attach(0, q, PORT_EA, 0); failed(PS3_SPURS_RC_NULL, 0, "NULL spurs");
    r.attach(INSTANCE1, q, 0, 0); failed(PS3_SPURS_RC_NULL, 0, "NULL port");
    r.attach(INSTANCE1 + 0x10, q, 0, 0); failed(PS3_SPURS_RC_NULL, 0, "NULL beats ALIGN");
    r.attach(INSTANCE1 + 0x10, q, PORT_EA, 0); failed(PS3_SPURS_RC_ALIGN, 0, "ALIGN before the owner lookup");
    r.attach(INSTANCE2 + 0x10, q, PORT_EA, 0); failed(PS3_SPURS_RC_ALIGN, 0, "ALIGN on an unowned address");
    r.attach(INSTANCE2, q, PORT_EA, 0); stopped(StartupStop::SpursAttachStop, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_ATTACH_STEP_OWNER, 0, "unknown instance");
    r.put32(INSTANCE1 + OFF_EXCEPTION, 7); r.attach(INSTANCE1, q, PORT_EA, 0); failed(PS3_SPURS_RC_STAT, 1, "exception queue set"); r.put32(INSTANCE1 + OFF_EXCEPTION, 0);
    r.attach(INSTANCE1, q, PORT_EA, UINT64_C(0x100000000)); CHECK(c.stop == StartupStop::None && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_OK);   // low 32 bits are 0: static
}
// g12c: the DYNAMIC port mode through the real dispatcher and the real composition. The mask is
// libsre's 0xFFFFFFFFFFFF0000, the kernel picks the lowest free port in 16..63, and the keyed path makes NO guest DA8 write.
void run_dynamic_mode() {
    Runtime r; r.boot(); const uint32_t q = r.make_queue(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const uint64_t prior = r.bits();
    // The port byte holds 0xFF, which the STATIC mode would refuse with INVAL: the dynamic path must not read it at all.
    r.put8(PORT_EA, 0xFF);
    // The port the firmware's request mask yields here: the lowest 16..63 slot with no live binding. The instance's own
    // internal queue already holds one, so it is derived from the real policy record rather than assumed.
    uint8_t want = 0xFF;
    {
        const ps3_spu_group_record *g = r.group();
        for (unsigned port = 16; port < PS3_SPU_PORTS && want == 0xFF; ++port) {
            bool taken = false;
            for (uint32_t t = 0; t < g->num; ++t) {
                ps3_lv2_queue probe{};
                taken = taken || (g->threads[t].port_queue[port] != 0 &&
                    ps3_lv2_queue_snapshot(&r.comp->lv2, g->threads[t].port_queue[port], &probe) == PS3_LV2_OK);
            }
            if (!taken) want = uint8_t(port);
        }
    }
    CHECK(want >= 16 && want < 64);
    const size_t from = r.published();
    r.attach(INSTANCE1, q, PORT_EA, 1);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && cpu.gpr[2] == TOC && cpu.gpr[1] == STACK);
    CHECK(c.spurs_attach_has_result && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_OK && c.spurs_attach_result.step == PS3_SPURS_ATTACH_STEP_DONE);
    CHECK(c.spurs_attach_info.spup == want && c.spurs_attach_info.port_written == 1 && c.spurs_attach_info.cas_attempts == 0 && c.spurs_attach_info.commit_status == 0);
    CHECK(r.get(PORT_EA, 1) == want);   // the CHOSEN port, not the 0xFF that was there
    CHECK(r.bound_threads(want, q) == 5);
    CHECK(r.instance().user_port_bits == (UINT64_C(1) << want) && r.instance().user_port_queue[want] == q);
    CHECK(r.bits() == prior);   // NO guest DA8 write: the firmware's keyed path skips the OR at 0xB074/0xB080
    // Memory traffic: the exception read (4 B) and the port-byte write (1 B). No port read, no DA8 seed read, no CAS.
    CHECK(c.spurs_attach_reads == 1 && c.spurs_attach_writes == 1 && c.spurs_attach_cas_calls == 0 && c.spurs_attach_cas_committed == 0);
    const auto ev = r.since(from);
    CHECK(ev.size() == 3);   // exception read, port write, TOC write
    if (ev.size() == 3) {
        CHECK(is_event(ev[0], false, INSTANCE1 + OFF_EXCEPTION, 4, 0));
        CHECK(is_event(ev[1], true, PORT_EA, 1, want));
        CHECK(is_event(ev[2], true, TOC_SLOT, 8, TOC));
    }
    CHECK(r.instance().state == PS3_SPURS_INSTANCE_ACTIVE);
    // The SAME queue on the same (now occupied) mask: EISCONN on a still-free port would bind it twice, so the second call
    // takes the next free port instead -- and a THIRD call on a full mask is EISCONN. Only the first two are meaningful.
    r.attach(INSTANCE1, q, PORT_EA, 1);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && c.spurs_attach_info.spup > want);
}
void run_negative_lanes() {
    Runtime r; r.boot(); const uint32_t q = r.make_queue(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    Quiet before = quiet_of(r, q);
    auto no_effect = [&](bool after_two_reads) { return c.spurs_attach_writes == 0 && c.spurs_attach_cas_calls == 0 && unchanged(r, before, q) && (!after_two_reads || c.spurs_attach_reads == 2); };
    r.put8(PORT_EA, 64); before = quiet_of(r, q); r.attach(INSTANCE1, q, PORT_EA, 0);   // port > 63: INVAL
    CHECK(c.stop == StartupStop::None && c.spurs_attach_result.rc == PS3_SPURS_RC_INVAL && cpu.gpr[3] == rc_reg(PS3_SPURS_RC_INVAL) && no_effect(true));
    r.put8(PORT_EA, 1); before = quiet_of(r, q);
    r.attach(INSTANCE1, ABSENT_QUEUE, PORT_EA, 0);   // absent queue: ESRCH BEFORE the DA8 seed read and before the connect
    CHECK(c.stop == StartupStop::None && c.spurs_attach_result.outcome == PS3_SPURS_OUTCOME_FAILED && c.spurs_attach_result.rc == uint32_t(PS3_LV2_ESRCH) &&
          c.spurs_attach_result.step == PS3_SPURS_ATTACH_STEP_QUEUE && cpu.gpr[3] == rc_reg(uint32_t(PS3_LV2_ESRCH)) && c.spurs_attach_reads == 2 && no_effect(true));
    r.attach(INSTANCE1, INTERNAL_QUEUE, PORT_EA, 0);   // SPURS's own service-bound queue: unsupported STOP, never bound
    CHECK(c.stop == StartupStop::SpursAttachStop && c.spurs_attach_result.reason == PS3_SPURS_STOP_UNSUPPORTED_LV2 && c.spurs_attach_result.step == PS3_SPURS_ATTACH_STEP_QUEUE &&
          r.instance().state == PS3_SPURS_INSTANCE_ACTIVE && unchanged(r, before, q) && c.spurs_attach_writes == 0);
    // invalid guest pointer (unmapped): the read is refused pre-effect; the stop reports the address and nothing was bound
    Runtime p; p.boot(); const uint32_t pq = p.make_queue(); const Quiet pbefore = quiet_of(p, pq);
    p.attach(INSTANCE1, pq, 0x30000000, 0);
    CHECK(p.control->stop == StartupStop::InvalidMemory && p.control->invalid_address == 0x30000000 && p.control->spurs_attach_stop_latched == StartupStop::InvalidMemory &&
          p.control->spurs_attach_result.reason == PS3_SPURS_STOP_READ_FAILED && p.control->spurs_attach_reads == 1 && unchanged(p, pbefore, pq) && p.instance().state == PS3_SPURS_INSTANCE_ACTIVE);
    // invalid TOC slot: preflighted before any callback
    Runtime t; t.boot(); const uint32_t tq = t.make_queue(); const Quiet tbefore = quiet_of(t, tq);
    t.attach(INSTANCE1, tq, PORT_EA, 0, 0x30000000);
    CHECK(t.control->stop == StartupStop::InvalidMemory && !t.control->spurs_attach_has_result && t.control->spurs_attach_reads == 0 && unchanged(t, tbefore, tq));
    // cancel requested BEFORE the call: stops before any callback
    Runtime k; k.boot(); const uint32_t kq = k.make_queue(); const Quiet kbefore = quiet_of(k, kq);
    k.control->cancelled.store(true); k.attach(INSTANCE1, kq, PORT_EA, 0); k.control->cancelled.store(false);
    CHECK(k.control->stop == StartupStop::Cancelled && !k.control->spurs_attach_has_result && k.control->spurs_attach_reads == 0 && unchanged(k, kbefore, kq));
}
// Trace budget around every REAL effect. `leave` = publications still available when the call starts (real recorder credit).
void run_trace_budget() {
    struct Lane { size_t leave; StartupStop stop; uint32_t reads, writes, cas; bool bound, port_written, state_retained, toc, flag; const char *name; };
    const Lane lanes[] = {
        {0, StartupStop::TraceCapacity, 0, 0, 0, false, false, false, false, false, "no credit: TOC preflight, before any callback"},
        {1, StartupStop::TraceCapacity, 1, 0, 0, false, false, false, false, true, "second read refused pre-effect"},
        {4, StartupStop::TraceCapacity, 3, 1, 0, false, true, true, false, true, "CAS refused pre-effect after the port byte: exact rollback, byte residue"},
        {5, StartupStop::TraceCapacity, 3, 1, 1, true, true, false, false, false, "CAS committed, TOC trace refused: bindings retained, r3 unpublished"},
        {6, StartupStop::None, 3, 1, 1, true, true, false, true, false, "exact credit: complete success"}};
    for (const Lane &lane : lanes) {
        Runtime r; r.boot(); const uint32_t q = r.make_queue(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
        unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8);
        const uint64_t prior = r.bits(); r.put8(PORT_EA, 3); const uint64_t spurs_before = INSTANCE1; r.leave_trace_capacity(lane.leave);
        r.attach(spurs_before, q, PORT_EA, 0);
        const bool bound = r.bound_threads(3, q) == 5, residue = c.spurs_attach_info.port_written != 0;
        const bool ok = c.stop == lane.stop && c.spurs_attach_reads == lane.reads && c.spurs_attach_writes == lane.writes && c.spurs_attach_cas_calls == lane.cas &&
            bound == lane.bound && residue == lane.port_written && (r.instance().state == PS3_SPURS_INSTANCE_RETAINED) == lane.state_retained &&
            (r.get(TOC_SLOT, 8) == TOC) == lane.toc && r.bits() == (lane.bound ? (prior | 8) : prior) &&
            (lane.toc ? cpu.gpr[3] == 0 : cpu.gpr[3] == INSTANCE1) && c.spurs_attach_trace_failure == lane.flag &&
            (lane.bound ? r.instance().user_port_bits == 8 : r.instance().user_port_bits == 0);
        if (!ok) std::printf("trace lane failed: %s (stop=%d reads=%u writes=%u cas=%u bound=%d residue=%d state=%d)\n", lane.name, int(c.stop), c.spurs_attach_reads,
            c.spurs_attach_writes, c.spurs_attach_cas_calls, int(bound), int(residue), int(r.instance().state));
        CHECK(ok);
    }
}
// Deadline polled before EVERY effect and once more after the domain returns. armGetSystemTick is the poll counter: with
// deadline_tick = k the k-th poll stops (poll 0 is the dispatcher pre-check, 1..3 the reads, 4 the port write, 5 the CAS, 6 the post-domain check).
void run_deadline_sweep() {
    uint64_t total = 0;
    {   // calibrate on a real call: the number of polls (dispatcher pre-checks + 3 reads + write + CAS + the post-domain check)
        Runtime r; r.boot(); const uint32_t q = r.make_queue(); r.control->deadline_tick = UINT64_MAX; tick_calls = 0;
        r.attach(INSTANCE1, q, PORT_EA, 0); total = tick_calls; r.control->deadline_tick = 0;
        CHECK(r.control->stop == StartupStop::None && total >= 7);
    }
    const uint64_t pre = total - 6;   // polls before the first callback
    for (uint64_t j = 1; j <= 7; ++j) {   // j: 1..3 reads, 4 write, 5 CAS, 6 post-domain check, 7 never stops
        const uint64_t k = j + pre - 1;
        Runtime r; r.boot(); const uint32_t q = r.make_queue(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
        unsigned char sentinel[8]; std::memset(sentinel, 0x5A, 8); r.put(TOC_SLOT, sentinel, 8);
        const uint64_t prior = r.bits(); c.deadline_tick = k; tick_calls = 0;
        r.attach(INSTANCE1, q, PORT_EA, 0); c.deadline_tick = 0;
        const bool completed = j == 7, committed = j >= 6, wrote = j >= 5, rolled_back = j == 4 || j == 5;
        const bool ok = (c.stop == (completed ? StartupStop::None : StartupStop::Deadline)) && c.spurs_attach_cas_committed == (committed ? 1u : 0u) &&
            c.spurs_attach_writes == (wrote ? 1u : 0u) && (r.bits() == (committed ? (prior | 2) : prior)) && (r.bound_threads(1, q) == 5) == committed &&
            (r.instance().state == PS3_SPURS_INSTANCE_RETAINED) == rolled_back && (r.get(TOC_SLOT, 8) == TOC) == completed && (completed ? cpu.gpr[3] == 0 : cpu.gpr[3] == INSTANCE1) &&
            c.spurs_attach_info.port_written == (wrote ? 1u : 0u) && (j <= 5 ? c.spurs_attach_stop_latched == StartupStop::Deadline : true) &&
            r.instance().user_port_bits == (committed ? 2u : 0u) && c.spurs_attach_reads == (j >= 4 ? 3u : uint32_t(j - 1));
        if (!ok) std::printf("deadline sweep j=%llu (k=%llu pre=%llu) failed: stop=%d reads=%u writes=%u cas=%u bits=%llx state=%d\n", (unsigned long long)j, (unsigned long long)k, (unsigned long long)pre, int(c.stop), c.spurs_attach_reads, c.spurs_attach_writes, c.spurs_attach_cas_committed, (unsigned long long)r.bits(), int(r.instance().state));
        CHECK(ok);
    }
}
// The committed-then-failed mapping is not reachable through the real recorder (a credited publish cannot fail), so the pure mapping is
// exercised on the exact domain result shapes the real domain produces for it (WRITE_FAILED at STEP_COMMITTED) and on a defensive OK+commit_status.
void run_classification() {
    ps3_spurs_result res{}; ps3_spurs_attach_info info{}; SpursAttachMemory ctx{nullptr};
    res.outcome = PS3_SPURS_OUTCOME_STOP; res.reason = PS3_SPURS_STOP_WRITE_FAILED; res.step = PS3_SPURS_ATTACH_STEP_COMMITTED; info.commit_status = 7;
    CHECK(spurs_attach_classify(res, info, ctx).disposition == SpursAttachDisposition::Stop && spurs_attach_classify(res, info, ctx).stop == StartupStop::SpursAttachStop);
    res = {}; res.outcome = PS3_SPURS_OUTCOME_OK;   // an OK that still carries a commit error is never a guest OK
    CHECK(spurs_attach_classify(res, info, ctx).disposition == SpursAttachDisposition::Stop);
    info = {}; CHECK(spurs_attach_classify(res, info, ctx).disposition == SpursAttachDisposition::PublishOk);
    ctx.trace_failure = true; CHECK(spurs_attach_classify(res, info, ctx).stop == StartupStop::TraceCapacity);   // committed + trace failure: stop, whatever the domain said
    ctx.trace_failure = false; ctx.latched = StartupStop::Cancelled; CHECK(spurs_attach_classify(res, info, ctx).stop == StartupStop::Cancelled);
    ctx.latched = StartupStop::None; res.outcome = PS3_SPURS_OUTCOME_FAILED; CHECK(spurs_attach_classify(res, info, ctx).disposition == SpursAttachDisposition::PublishFailed);
    res.outcome = PS3_SPURS_OUTCOME_STOP; res.reason = PS3_SPURS_STOP_KERNEL; CHECK(spurs_attach_classify(res, info, ctx).stop == StartupStop::SpursKernelFence);
}
}
int main() {
    std::printf("SPURS_ATTACH_LAYOUT startup_owner=%zu control=%zu max_cas_attempts=%u\n", sizeof(StartupExecution), sizeof(StartupControl), STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS);
    run_happy_and_exact_trace();
    run_cas_contention();
    run_sdk_policy();
    run_validation();
    run_dynamic_mode();
    run_negative_lanes();
    run_trace_budget();
    run_deadline_sweep();
    run_classification();
    std::printf("SPURS_ATTACH_HOST failures=%d native_runtime=false game_boot=false\n", failures);
    return failures != 0;
}

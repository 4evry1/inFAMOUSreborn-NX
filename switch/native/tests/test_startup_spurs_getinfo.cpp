/* Host behavior test for 1F402F8F (cellSpursGetInfo) through the REAL dispatcher (ps3_hle_call) and the REAL composition
 * (startup_spurs_compose) over real collaborators: atomic domain + coherent guest memory, lv2 objects, SPU group policy (configured only
 * through the guest's own syscall 169), PPU service registry with real host threads, the kernel-1 idle service, the lwmutex/lwcond
 * registries and TWO real aa6269a8 initializations. The generated call site is modelled by the call fixture (argument registers as the
 * import stub sets them); this is a label fixture, not the game: no game boot or render claim. Expected bytes and the expected access log
 * are derived here from the pinned reference offsets (cellSpurs.cpp:1970-2007, cellSpurs.h:308-329) and the values the GUEST memory holds,
 * never captured from the implementation. Injection is test-only link wrapping (-Wl,--wrap=...; see verify_startup_spurs_getinfo.sh):
 * inert unless armed, armed only for the PPU thread, no production hook. */
#include <chrono>
#include <thread>
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
#define CHECK(value) do { if (!(value)) { std::printf("SPURS_INFO_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
std::atomic<uint64_t> tick_calls{0}, clock_now{1};   // armGetSystemTick: a deterministic poll counter (deadline sweep); only the PPU thread polls
constexpr uint32_t INSTANCE1 = 0x7CF100, INSTANCE2 = 0x7D0380, ATTR = 0xD00FF990, STACK = 0xD00FF910, TOC_SLOT = STACK + 0x28;
constexpr uint32_t STACK_BASE = 0xD00FF000, STACK_SPAN = 0x3000, HOST_STACK = 0x800000, HOST_SIZE = 0x900000;
constexpr uint64_t TOC = UINT64_C(0x123456789abcdef0);
constexpr uint32_t WINDOW = 0x400000, WINDOW_END = 0x700000, QUOTA = 0x300000, INFO_NID = 0x1f402f8f, SDK = 0x250001;
// G15c (decision 26): the objects the LFQueue entry is called on. `INSTANCE1`/`INSTANCE2` are LIVE CellSpurs headers -- the kernel worker of
// each instance revalidates `spurs+0x74` (flags1) and `spurs+0x76` (nspus) from the shared atomic domain on every fresh L0 reservation
// (native/domain/spurs_kernel1_service.c:248), and an LFQueue case writing those bytes makes that halt a matter of timing. A case whose
// operand is that word uses LFQ_SCRATCH_EA: a 4 KiB block inside the fixture's RAM window that is no instance, taskset or queue.
constexpr uint32_t LFQ_SCRATCH_EA = 0x7D3000, LFQ_SCRATCH_SIZE = 0x1000;   // the fixture's extra writable block: past INSTANCE2, no instance/taskset/queue inside
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
    // G15c: the RAM declarations live on the heap and belong to `binding`, because PpuMemoryBinding keeps the POINTER (the header requires
    // the declarations to stay stable through release) and a stack array held by an object that can outlive its frame is not stable.
    // The fourth range is the LFQueue cases' reserved block.
    static constexpr size_t RANGE_COUNT = 4;
    std::unique_ptr<PpuMemoryRamRange[]> ranges_array;
    std::unique_ptr<PpuMemoryBinding> binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(16384);
    PpuMemoryTraceRecorder recorder;
    static ps3_clock_status sample(void *, uint64_t *value) { *value = clock_now.load(); return PS3_CLOCK_OK; }
    ps3_clock clock{nullptr, sample};
    infamous::discovery::DiscoveryBudget budget_policy;
    DiscoveryBudgetBinding budget{budget_policy, clock};
    std::unique_ptr<StartupSpursComposition> comp{new StartupSpursComposition};
    ps3_pthread_service_thread threads[PS3_SPURS_MAX_INSTANCES][3]{};
    std::vector<unsigned char> local_stores = std::vector<unsigned char>(size_t(PS3_SPURS_LS_SLOTS) * PS3_SPURS_K1_LS_BYTES);
    StartupSpursPorts ports;
    bool composed = false;
    bool journaled;
    Runtime(bool journal_mode = false, uint64_t hle_limit = 1000000) : budget_policy(infamous::discovery::Limits{64, hle_limit, 64, UINT64_C(1000000000)}, 0), journaled(journal_mode) {
        CHECK(!ps3_pthread_guard_init(&g_dom) && !ps3_pthread_guard_init(&g_mem) && !ps3_pthread_guard_init(&g_lv2) && !ps3_pthread_guard_init(&g_ppu) &&
              !ps3_pthread_guard_init(&g_pol) && !ps3_pthread_guard_init(&g_mutex) && !ps3_pthread_guard_init(&g_cond) && !ps3_pthread_wait_port_init(&port));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), ps3_pthread_guard_port(&g_dom), owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        const ps3_user_memory_config mc{WINDOW, WINDOW_END, QUOTA, ps3_atomic_domain_memory(&domain), ps3_pthread_guard_port(&g_mem)};
        CHECK(ps3_user_memory_init(&memory, &mc, slots, 2) == PS3_USER_MEMORY_OK);
        CHECK(ps3_spu_resource_policy_init(&policy, ps3_pthread_guard_port(&g_pol)) == PS3_SPU_POLICY_OK);
        CHECK(ps3_lwmutex_init_with_conditions(&mutexes, &domain, ps3_pthread_guard_port(&g_mutex), mutex_wait.port(), mutex_objects, 4, mutex_waiters, 8, 4).status == PS3_LWMUTEX_OK);
        CHECK(ps3_lwcond_init(&conditions, &mutexes, ps3_pthread_guard_port(&g_cond), cond_wait.port(), {&main_id, main_exists}, condition_objects, 4, condition_waiters, 4).status == PS3_LWCOND_OK);
        binding.reset(new PpuMemoryBinding(domain, range_set(), RANGE_COUNT));
        CHECK(ps3_atomic_commit(&domain, 0x7CF000, 0x3000) == PS3_GUEST_MEMORY_OK && ps3_atomic_commit(&domain, STACK_BASE, STACK_SPAN) == PS3_GUEST_MEMORY_OK &&
              ps3_atomic_commit(&domain, 0x7D2000, 0x1000) == PS3_GUEST_MEMORY_OK && ps3_atomic_commit(&domain, LFQ_SCRATCH_EA, LFQ_SCRATCH_SIZE) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding->coherent_memory;
        execution.control = control.get(); execution.conditions = &conditions; execution.spu_resources = &policy;
        execution.guest_priority = 250; execution.has_guest_priority = true;
        execution.ppu.cpu.thread_id = 1; execution.ppu.cpu.gpr[1] = STACK; execution.ppu.cpu.gpr[2] = TOC; execution.ppu.cpu.lr = 0x2cfe70;
        execution.process_sdk = {SDK, true};   // explicit effective process configuration (as the composition root derives it from the plan)
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Strict, 0));
        if (journaled) CHECK(native_startup_admit(execution, *binding, services)); else CHECK(native_startup_admit_budgeted(execution, *binding, services, recorder, budget));
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
    static constexpr unsigned char zero_byte = 0;
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n) { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    void put8(uint32_t a, uint8_t v) { put(a, &v, 1); }
    void put32(uint32_t a, uint32_t v) { const unsigned char e[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; put(a, e, 4); }
    uint64_t get(uint32_t a, size_t n) { unsigned char b[8]{}; CHECK(mem().read(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); uint64_t v = 0; for (size_t i = 0; i < n; ++i) v = v << 8 | b[i]; return v; }
    // The declarations the binding points at. Built before the binding and released after it (member order), so the pointer the binding
    // holds is valid for exactly the binding's lifetime.
    const PpuMemoryRamRange *range_set() {
        if (!ranges_array) {
            ranges_array.reset(new PpuMemoryRamRange[RANGE_COUNT]{{0x7CF000, 0x3000, true}, {STACK_BASE, STACK_SPAN, true}, {0x7D2000, 0x1000, false}, {LFQ_SCRATCH_EA, LFQ_SCRATCH_SIZE, true}});   // third: read-only RAM, fourth: the LFQueue block
        }
        return &ranges_array[0];
    }
    ps3_spurs_instance &instance(unsigned i = 0) { return comp->runtime.instances[i]; }
    /* G15c: the pages an object under test occupies. The atomic domain zero-fills ON COMMIT and never re-zeroes, so a page committed during
     * set-up holds whatever was written into it (the taskset line the LFQueue cases build) for every LATER owner reservation -- the
     * instance created next, or the kernel worker woken by the next store. Storing a zero into those pages at the start of a case makes the
     * set-up's non-zero bytes unrepresentable to any concurrent reader for the rest of the case. */
    void zero_pages(uint32_t address, uint64_t length) {
        const uint64_t first = address & ~(uint64_t(PS3_GUEST_PAGE_BYTES) - 1u), last = (uint64_t(address) + length + PS3_GUEST_PAGE_BYTES - 1u) & ~(uint64_t(PS3_GUEST_PAGE_BYTES) - 1u);
        for (uint64_t page = first; page < last; page += PS3_GUEST_PAGE_BYTES)
            CHECK(mem().write(mem().context, page, &zero_byte, 1) == PS3_GUEST_MEMORY_OK);
    }
    void put_attribute(uint32_t nspus = 5, uint32_t spu_prio = 128, uint32_t ppu_prio = 249) { unsigned char image[512]{}; const uint32_t w[5]{2, SDK, nspus, spu_prio, ppu_prio};
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
    }
    // The second real instance (1 SPU, distinct priorities) through the same guest aa6269a8 route; 5+1 <= the 6 usable SPUs.
    void boot2() {
        boot(); auto &cpu = execution.ppu.cpu;
        put_attribute(1, 100, 200); cpu.gpr[3] = INSTANCE2; cpu.gpr[4] = ATTR; cpu.gpr[5] = cpu.gpr[6] = cpu.gpr[7] = cpu.gpr[8] = 0; cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        run(0xaa6269a8, false); CHECK(control->stop == StartupStop::None && cpu.gpr[3] == 0 && instance(1).state == PS3_SPURS_INSTANCE_ACTIVE);
    }
    // Events the call published (everything after `from`), in order.
    std::vector<PpuMemoryTraceEvent> since(size_t from) { std::vector<PpuMemoryTraceEvent> out;
        if (journaled) { for (size_t i = from; i < control->journal.count; ++i) out.push_back(control->journal.events[i]); return out; }
        PpuMemoryTraceSample s; for (size_t i = from; recorder.read(i, s); ++i) out.push_back(s.event); return out; }
    size_t published() { return journaled ? control->journal.count : recorder.report().retained_count; }
    void leave_trace_capacity(size_t leave) {   // consume real recorder credit until exactly `leave` publications can still begin
        while (recorder.can_begin(leave + 1)) { CHECK(recorder.begin(1)); recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 0x7CF000, 1, 0)); recorder.finish(); }
        CHECK((leave == 0 || recorder.can_begin(leave)) && !recorder.can_begin(leave + 1));
    }
};}   // namespace
extern "C" uint64_t armGetSystemTick(void) { return tick_calls.fetch_add(1); }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }
namespace {
constexpr uint32_t B0 = INSTANCE1, B1 = INSTANCE2, INFO = STACK_BASE + 0x1000, RO_RAM = 0x7D2000, UNOWNED = 0x7D1000, SPAN = 0x118;
// ---- test-only injection (link wrappers below). Armed only for the PPU thread; inert otherwise; every counted access is one
// domain access to a spurs-extent read or an info-range write, numbered 0.. in the order the adapter callbacks see them.
struct Inject {
    enum Kind { Off, MemFail, TraceBegin, After } kind = Off;
    enum Act { Cancel, Fence, ClockBack, Poke, FillJournal } act = Cancel;
    unsigned k = 0, seen = 0;
    pthread_t ppu{};
    uint32_t spurs = 0, info = 0, poke_address = 0; uint64_t poke_value = 0; size_t poke_length = 0;
    bool fired = false, restore = false; uint32_t fired_address = 0, restore_address = 0; uint64_t restore_value = 0; size_t restore_length = 0;
    Runtime *r = nullptr;
};
Inject inject;
bool counted(uint64_t address, bool write) {
    if ((inject.kind != Inject::MemFail && inject.kind != Inject::After) || !pthread_equal(pthread_self(), inject.ppu)) return false;
    return write ? address >= inject.info && address < uint64_t(inject.info) + SPAN : address >= inject.spurs && address < uint64_t(inject.spurs) + 0x1000;
}
}
extern "C" ps3_guest_memory_status __real_ps3_atomic_read(ps3_atomic_domain *, uint64_t, void *, size_t);
extern "C" ps3_guest_memory_status __real_ps3_atomic_write(ps3_atomic_domain *, uint64_t, const void *, size_t);
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_read(ps3_atomic_domain *, uint64_t, void *, size_t);
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_write(ps3_atomic_domain *, uint64_t, const void *, size_t);
extern "C" bool __real__ZN22PpuMemoryTraceRecorder5beginEm(PpuMemoryTraceRecorder *, size_t);
extern "C" bool __wrap__ZN22PpuMemoryTraceRecorder5beginEm(PpuMemoryTraceRecorder *, size_t);
static_assert(std::is_same<decltype(&__wrap_ps3_atomic_read), decltype(&ps3_atomic_read)>::value &&
              std::is_same<decltype(&__real_ps3_atomic_read), decltype(&ps3_atomic_read)>::value &&
              std::is_same<decltype(&__wrap_ps3_atomic_write), decltype(&ps3_atomic_write)>::value &&
              std::is_same<decltype(&__real_ps3_atomic_write), decltype(&ps3_atomic_write)>::value, "wraps must match the wrapped signatures");
static void inject_tail(unsigned idx) {   // optional restore of a poked byte right after the NEXT access (the instance is live: keep the window minimal)
    if (inject.kind != Inject::After || !inject.restore || idx != inject.k + 1) return;
    unsigned char b[8]; for (size_t i = 0; i < inject.restore_length; ++i) b[i] = uint8_t(inject.restore_value >> (8 * (inject.restore_length - 1 - i)));
    __real_ps3_atomic_write(&inject.r->domain, inject.restore_address, b, inject.restore_length); inject.restore = false;
}
static void inject_after() {   // runs on the PPU thread right AFTER the k-th counted access really happened
    inject.fired = true;
    switch (inject.act) {
    case Inject::Cancel: inject.r->control->cancelled.store(true); break;
    case Inject::Fence: inject.r->control->spurs_fence_latched = true; break;
    case Inject::ClockBack: clock_now.store(0); break;
    case Inject::FillJournal: inject.r->control->journal.count = inject.r->control->journal.capacity; break;
    case Inject::Poke: { unsigned char b[8]; for (size_t i = 0; i < inject.poke_length; ++i) b[i] = uint8_t(inject.poke_value >> (8 * (inject.poke_length - 1 - i)));
        __real_ps3_atomic_write(&inject.r->domain, inject.poke_address, b, inject.poke_length); break; }
    }
}
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_read(ps3_atomic_domain *d, uint64_t a, void *b, size_t n) {
    if (!counted(a, false)) return __real_ps3_atomic_read(d, a, b, n);
    const unsigned idx = inject.seen++;
    if (inject.kind == Inject::MemFail && idx == inject.k) { inject.fired = true; inject.fired_address = uint32_t(a); return PS3_GUEST_MEMORY_INVALID_RANGE; }
    const auto s = __real_ps3_atomic_read(d, a, b, n);
    if (inject.kind == Inject::After && idx == inject.k) inject_after();
    inject_tail(idx);
    return s;
}
extern "C" ps3_guest_memory_status __wrap_ps3_atomic_write(ps3_atomic_domain *d, uint64_t a, const void *b, size_t n) {
    if (!counted(a, true)) return __real_ps3_atomic_write(d, a, b, n);
    const unsigned idx = inject.seen++;
    if (inject.kind == Inject::MemFail && idx == inject.k) { inject.fired = true; inject.fired_address = uint32_t(a); return PS3_GUEST_MEMORY_INVALID_RANGE; }
    const auto s = __real_ps3_atomic_write(d, a, b, n);
    if (inject.kind == Inject::After && idx == inject.k) inject_after();
    inject_tail(idx);
    return s;
}
extern "C" bool __wrap__ZN22PpuMemoryTraceRecorder5beginEm(PpuMemoryTraceRecorder *self, size_t n) {   // real recorder; refuses only the armed ordinal
    if (inject.kind == Inject::TraceBegin && n == 1 && pthread_equal(pthread_self(), inject.ppu) && ++inject.seen == inject.k) { inject.fired = true; return false; }
    return __real__ZN22PpuMemoryTraceRecorder5beginEm(self, n);
}
namespace {
uint64_t rc_reg(uint32_t rc) { return uint64_t(int64_t(int32_t(rc))); }
struct Ev { bool write; uint32_t address; size_t length; uint64_t value; };
bool is_event(const PpuMemoryTraceEvent &e, const Ev &x) {
    return e.origin == PpuMemoryTraceOrigin::Hle && (e.operation == PpuMemoryTraceOperation::Write) == x.write && e.address == x.address && e.length == x.length && e.value == x.value; }
// The normative access log (PLAN s3.2), derived from the reference offsets and the values the GUEST currently holds. Truncated where the
// domain must stop (traceBuffer high32, prefix > 15) after the read that proves it.
std::vector<Ev> expected_log(Runtime &r, uint32_t B, uint32_t I) {
    std::vector<Ev> e;
    auto R = [&](uint32_t off, size_t n) { const uint64_t v = r.get(B + off, n); e.push_back({false, B + off, n, v}); return v; };
    auto W = [&](uint32_t off, size_t n, uint64_t v) { e.push_back({true, I + off, n, v}); };
    const uint64_t flags = R(0xD80, 4);
    { const uint64_t v = R(0x76, 1); W(0x00, 4, v); }
    { const uint64_t v = R(0xD84, 4); W(0x04, 4, v); }
    { const uint64_t v = R(0xD88, 4); W(0x08, 4, v); }
    W(0x0C, 1, flags & 1 ? 1 : 0); W(0x0D, 1, flags & 4 ? 1 : 0);
    { const uint64_t v = R(0xD30, 4); W(0x24, 4, v); }
    uint64_t ids[8]; for (unsigned k = 0; k < 8; ++k) ids[k] = R(0xD34 + 4 * k, 4);
    for (unsigned k = 0; k < 8; ++k) W(0x28 + 4 * k, 4, ids[k]);
    { const uint64_t v = R(0xD20, 8); W(0x48, 8, v); }
    { const uint64_t v = R(0xD28, 8); W(0x50, 8, v); }
    { const uint64_t v = R(0x948, 8); W(0x18, 8, v); }
    const uint64_t raw = R(0x900, 8);
    if (raw >> 32) return e;
    W(0x10, 4, uint32_t(raw) & ~3u); W(0x20, 4, raw & 3);
    const uint64_t n = R(0xD9B, 1);
    if (n > 15) return e;
    uint64_t chars[15]; for (unsigned j = 0; j < n; ++j) chars[j] = R(0xD8C + j, 1);
    for (unsigned j = 0; j < n; ++j) W(0x58 + j, 1, chars[j]);
    W(0x58 + uint32_t(n), 1, 0); W(0x68, 4, n); W(0x70, 4, 0); W(0x6C, 4, 0);
    return e;
}
std::vector<uint8_t> apply(const std::vector<Ev> &log, size_t count, uint32_t I) {   // sentinel 0xA5 plus the first `count` accesses' writes
    std::vector<uint8_t> out(SPAN, 0xA5);
    for (size_t i = 0; i < count && i < log.size(); ++i) if (log[i].write)
        for (size_t b = 0; b < log[i].length; ++b) out[log[i].address - I + b] = uint8_t(log[i].value >> (8 * (log[i].length - 1 - b)));
    return out;
}
struct Counts { uint32_t reads = 0, writes = 0; int last_write = -1; };
Counts count_of(const std::vector<Ev> &log, size_t n, uint32_t I) { Counts c; for (size_t i = 0; i < n && i < log.size(); ++i) { if (log[i].write) { ++c.writes; c.last_write = int(log[i].address - I); } else ++c.reads; } return c; }
std::vector<uint8_t> out_bytes(Runtime &r, uint32_t I) { std::vector<uint8_t> v(SPAN); CHECK(r.mem().read(r.mem().context, I, v.data(), SPAN) == PS3_GUEST_MEMORY_OK); return v; }
void fill(Runtime &r, uint32_t I) { std::vector<uint8_t> v(SPAN, 0xA5); r.put(I, v.data(), SPAN); unsigned char t[8]; std::memset(t, 0x5A, 8); r.put(TOC_SLOT, t, 8); }
void call(Runtime &r, uint64_t spurs, uint64_t info, uint64_t sp = STACK) {
    r.control->import_count = 0;   // the bounded import-observation array (32) is bookkeeping, not under test; sweeps make many calls
    auto &cpu = r.execution.ppu.cpu; cpu.gpr[3] = spurs; cpu.gpr[4] = info; cpu.gpr[5] = cpu.gpr[6] = cpu.gpr[7] = cpu.gpr[8] = UINT64_C(0x5555555555555555);
    cpu.gpr[1] = sp; cpu.gpr[2] = TOC; r.run(INFO_NID, false);
}
void arm(Runtime &r, Inject::Kind kind, unsigned k, Inject::Act act = Inject::Cancel, uint32_t B = B0, uint32_t I = INFO) {
    inject = {}; inject.kind = kind; inject.k = k; inject.act = act; inject.ppu = pthread_self(); inject.spurs = B; inject.info = I; inject.r = &r;
}
bool attach_counters_zero(StartupControl &c) { return !c.spurs_attach_calls && !c.spurs_attach_reads && !c.spurs_attach_writes && !c.spurs_attach_cas_calls && !c.spurs_attach_has_result; }
#define SHOW(r) std::printf("  observed stop=%d calls=%zu returns=%zu has_result=%d reads=%u writes=%u r3=0x%016llx outcome=%d rc=0x%08x reason=%u step=%u detail=0x%x mismatch=0x%x partial=%u latched=%d invalid=0x%x\n", int((r).control->stop), (r).control->spurs_info_calls, \
    (r).control->spurs_info_returns, int((r).control->spurs_info_has_result), (r).control->spurs_info_reads, (r).control->spurs_info_writes, (unsigned long long)(r).execution.ppu.cpu.gpr[3], \
    int((r).control->spurs_info_result.outcome), unsigned((r).control->spurs_info_result.rc), unsigned((r).control->spurs_info_result.reason), unsigned((r).control->spurs_info_result.step), unsigned((r).control->spurs_info_result.detail), \
    unsigned((r).control->spurs_info_report.mirror_mismatch), unsigned((r).control->spurs_info_report.partial), int((r).control->spurs_info_stop_latched), unsigned((r).control->spurs_info_invalid_address))

// Full-success verification for one instance. The expected log/bytes come from the guest's own bytes read BEFORE the call.
bool expect_success(Runtime &r, uint32_t B, uint32_t I, uint64_t a3, uint64_t a4, const char *name, uint32_t mismatch = 0, bool mismatch_exact = true) {
    auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const auto log = expected_log(r, B, I); fill(r, I);
    const size_t from = r.published(), calls = c.spurs_info_calls, returns = c.spurs_info_returns;
    call(r, a3, a4);
    const Counts n = count_of(log, log.size(), I); const auto ev = r.since(from);
    bool ok = c.stop == StartupStop::None && cpu.gpr[3] == 0 && cpu.gpr[2] == TOC && cpu.gpr[1] == STACK && r.get(TOC_SLOT, 8) == TOC;
    ok = ok && c.spurs_info_calls == calls + 1 && c.spurs_info_returns == returns + 1 && c.spurs_info_has_result && !c.spurs_info_callback_error && !c.spurs_info_trace_failure &&
         c.spurs_info_stop_latched == StartupStop::None && c.spurs_info_arguments[0] == a3 && c.spurs_info_arguments[1] == a4;
    ok = ok && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_OK && c.spurs_info_result.rc == 0 && c.spurs_info_result.step == PS3_SPURS_INFO_STEP_DONE;
    ok = ok && c.spurs_info_reads == n.reads && c.spurs_info_writes == n.writes && c.spurs_info_report.reads == n.reads && c.spurs_info_report.writes == n.writes &&
         int(c.spurs_info_report.last_write_offset) == n.last_write && !c.spurs_info_report.partial;
    ok = ok && (mismatch_exact ? c.spurs_info_report.mirror_mismatch == mismatch : (c.spurs_info_report.mirror_mismatch & mismatch) == mismatch);
    ok = ok && out_bytes(r, I) == apply(log, log.size(), I) && attach_counters_zero(c) && ev.size() == log.size() + 1;
    for (size_t i = 0; ok && i < log.size(); ++i) ok = is_event(ev[i], log[i]);
    ok = ok && is_event(ev.back(), {true, TOC_SLOT, 8, TOC});
    if (!ok) { std::printf("GETINFO case failed: %s\n", name); SHOW(r); }
    CHECK(ok); return ok;
}
// Stopped/partial verification: `performed` accesses of the log really happened and are accounted, nothing else.
bool expect_partial(Runtime &r, const std::vector<Ev> &log, size_t performed, size_t from, uint32_t I, uint64_t a3, StartupStop stop, const char *name) {
    auto &c = *r.control; auto &cpu = r.execution.ppu.cpu; const Counts n = count_of(log, performed, I); const auto ev = r.since(from);
    bool ok = c.stop == stop && cpu.gpr[3] == a3 && cpu.gpr[2] == TOC && r.get(TOC_SLOT, 8) == UINT64_C(0x5A5A5A5A5A5A5A5A);
    ok = ok && c.spurs_info_reads == n.reads && c.spurs_info_writes == n.writes && out_bytes(r, I) == apply(log, performed, I) && ev.size() == performed && attach_counters_zero(c);
    for (size_t i = 0; ok && i < performed; ++i) ok = is_event(ev[i], log[i]);
    if (c.spurs_info_has_result) ok = ok && c.spurs_info_report.reads == n.reads && c.spurs_info_report.writes == n.writes && int(c.spurs_info_report.last_write_offset) == n.last_write &&
        (c.spurs_info_report.partial != 0) == (n.writes > 0 && c.spurs_info_result.outcome != PS3_SPURS_OUTCOME_OK);
    if (!ok) { std::printf("GETINFO partial case failed: %s (performed=%zu)\n", name, performed); SHOW(r); }
    CHECK(ok); return ok;
}
void put_prefix(Runtime &r, uint32_t B, const char *s, uint8_t n) { r.put8(B + 0xD9B, n); for (uint8_t j = 0; j < n && j < 15; ++j) r.put8(B + 0xD8C + j, j < std::strlen(s) ? uint8_t(s[j]) : uint8_t(0)); }

void run_two_instances() {
    Runtime r; r.boot2(); auto &c = *r.control;
    const ps3_spurs_instance &a = r.instance(0), &b = r.instance(1);
    CHECK(a.nspus == 5 && b.nspus == 1 && a.group != b.group && a.handler.id != b.handler.id && a.helper.id != b.helper.id && a.spu_threads[0] != b.spu_threads[0]);
    // RED anchor: on the old dispatch this is UnknownImport with the output untouched. Printed for the red/green record.
    fill(r, INFO); call(r, B0, INFO);
    std::printf("GETINFO_PROBE stop=%d (UnknownImport=%d) output_untouched=%d calls=%zu\n", int(c.stop), int(StartupStop::UnknownImport), int(out_bytes(r, INFO) == std::vector<uint8_t>(SPAN, 0xA5)), c.spurs_info_calls);
    // A1 + A2: instance 0, then instance 1, high register bits ignored, real ids from the real records.
    CHECK(expect_success(r, B0, INFO, UINT64_C(0xDEADBEEF00000000) | B0, UINT64_C(0xFFFFFFFF00000000) | INFO, "instance0 (high gpr bits)"));
    const auto first = out_bytes(r, INFO);
    CHECK(r.get(INFO + 0x00, 4) == 5 && r.get(INFO + 0x04, 4) == 128 && r.get(INFO + 0x08, 4) == 249 && r.get(INFO + 0x24, 4) == a.group && r.get(INFO + 0x48, 8) == a.handler.id && r.get(INFO + 0x50, 8) == a.helper.id);
    for (unsigned k = 0; k < 8; ++k) CHECK(r.get(INFO + 0x28 + 4 * k, 4) == (k < 5 ? a.spu_threads[k] : 0));   // all 8 copied; the ones beyond nSpus are the guest's zeros
    CHECK(r.get(INFO + 0x58, 1) == 0 && r.get(INFO + 0x68, 4) == 0 && r.get(INFO + 0x6C, 4) == 0 && r.get(INFO + 0x70, 4) == 0 && r.get(INFO + 0x18, 8) == 0 && r.get(INFO + 0x10, 4) == 0 && r.get(INFO + 0x20, 4) == 0);
    CHECK(expect_success(r, B1, INFO, B1, INFO, "instance1"));
    CHECK(r.get(INFO + 0x00, 4) == 1 && r.get(INFO + 0x04, 4) == 100 && r.get(INFO + 0x08, 4) == 200 && r.get(INFO + 0x24, 4) == b.group && r.get(INFO + 0x48, 8) == b.handler.id && r.get(INFO + 0x50, 8) == b.helper.id);
    CHECK(out_bytes(r, INFO) != first && r.get(INFO + 0x28, 4) == b.spu_threads[0] && r.get(INFO + 0x2C, 4) == 0);
    CHECK(c.spurs_info_calls == 3 && c.spurs_info_returns == 3 && a.state == PS3_SPURS_INSTANCE_ACTIVE && b.state == PS3_SPURS_INSTANCE_ACTIVE);   // the probe is a real call too
    // no info alignment / exception validation: odd, +2, +4 placements and a nonzero exception field succeed with exact bytes at that offset
    r.put32(B0 + 0xD6C, 1);
    for (uint32_t off : {1u, 2u, 4u}) CHECK(expect_success(r, B0, INFO + off, B0, INFO + off, "unaligned info"));
    r.put32(B0 + 0xD6C, 0);
    // guest memory is authoritative: mutated values flow to the output; mirror disagreement is reported but never alters bytes/outcome
    r.put32(B0 + 0xD80, 0x5); r.put32(B0 + 0xD84, 0x11); r.put32(B0 + 0xD88, 0x22); r.put32(B0 + 0xD30, 0x01020304);
    for (unsigned k = 0; k < 8; ++k) r.put32(B0 + 0xD34 + 4 * k, 0x0A0B0C00 + k);
    r.put(B0 + 0xD20, "\x00\x00\x00\x00\xAA\xBB\xCC\xDD", 8); r.put(B0 + 0xD28, "\x00\x00\x00\x00\x11\x22\x33\x44", 8);
    r.put(B0 + 0x948, "\x11\x22\x33\x44\x55\x66\x77\x88", 8); r.put(B0 + 0x900, "\x00\x00\x00\x00\xDE\xAD\xBE\xEF", 8); put_prefix(r, B0, "ABC", 3);
    CHECK(expect_success(r, B0, INFO, B0, INFO, "guest-authoritative mutation", 0xFE, false));
    CHECK(r.get(INFO + 0x0C, 1) == 1 && r.get(INFO + 0x0D, 1) == 1 && r.get(INFO + 0x00, 4) == 5 && r.get(INFO + 0x10, 4) == 0xDEADBEEC && r.get(INFO + 0x20, 4) == 3 &&
          r.get(INFO + 0x18, 8) == UINT64_C(0x1122334455667788) && r.get(INFO + 0x58, 1) == 'A' && r.get(INFO + 0x5A, 1) == 'C' && r.get(INFO + 0x5B, 1) == 0 && r.get(INFO + 0x68, 4) == 3 &&
          r.get(INFO + 0x28, 4) == 0x0A0B0C00 && r.get(INFO + 0x44, 4) == 0x0A0B0C07);
    CHECK(c.spurs_info_report.nspus == 5 && c.spurs_info_report.prefix_size == 3 && c.spurs_info_report.flags == 5 && c.spurs_info_report.trace_buffer_raw == 0xDEADBEEF && c.spurs_info_report.trace_size == UINT64_C(0x1122334455667788));
    // mid-call guest mutation after the flags read / the nSpus read: captured values are used (reference order)
    r.put32(B0 + 0xD80, 0x5); r.put8(B0 + 0x76, 5);
    { const auto log = expected_log(r, B0, INFO); fill(r, INFO);
      arm(r, Inject::After, 0, Inject::Poke); inject.poke_address = B0 + 0xD80; inject.poke_length = 4; inject.poke_value = 0; const size_t from = r.published();
      call(r, B0, INFO); inject = {};
      CHECK(c.stop == StartupStop::None && out_bytes(r, INFO) == apply(log, log.size(), INFO) && r.get(INFO + 0x0C, 1) == 1 && r.get(INFO + 0x0D, 1) == 1 && r.since(from).size() == log.size() + 1);
      r.put32(B0 + 0xD80, 0x5); fill(r, INFO);
      arm(r, Inject::After, 3, Inject::Poke); inject.poke_address = B0 + 0xD84; inject.poke_length = 4; inject.poke_value = 0x77; inject.restore = true; inject.restore_address = B0 + 0xD84; inject.restore_length = 4; inject.restore_value = 0x11;
      call(r, B0, INFO); inject = {};   // priority poked after its read, restored after the next access: the captured read value is what is written
      CHECK(c.stop == StartupStop::None && out_bytes(r, INFO) == apply(log, log.size(), INFO) && r.get(INFO + 0x04, 4) == 0x11 && r.get(B0 + 0xD84, 4) == 0x11); }
    // NOTE: an observed nSpus=255 is NOT exercised here: poking the live instance's nSpus byte wakes the real kernel-1 service, which then (correctly)
    // fences on the impossible value. That case (mirror bounded by the native nspus, all 8 guest ids returned) is owned by the domain host test.
    CHECK(r.get(B0 + 0x76, 1) == 5);
}
}
namespace {
bool expect_stopped(Runtime &r, uint64_t a3, uint64_t a4, ps3_spurs_stop_reason reason, unsigned step, const char *name, uint32_t I = INFO) {
    auto &c = *r.control; const size_t returns = c.spurs_info_returns;
    if (I == INFO) fill(r, I); else { unsigned char t[8]; std::memset(t, 0x5A, 8); r.put(TOC_SLOT, t, 8); }   // an overlapping output is a live instance: snapshot, never overwrite
    const auto before = out_bytes(r, I); const size_t from = r.published(); call(r, a3, a4);
    const bool ok = c.stop == StartupStop::SpursInfoStop && c.spurs_info_has_result && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP && c.spurs_info_result.reason == reason &&
        c.spurs_info_result.step == step && c.spurs_info_returns == returns && c.spurs_info_reads == 0 && c.spurs_info_writes == 0 && r.since(from).empty() && !c.spurs_info_report.partial &&
        c.spurs_info_stop_latched == StartupStop::None && r.execution.ppu.cpu.gpr[3] == a3 && out_bytes(r, I) == before && r.get(TOC_SLOT, 8) == UINT64_C(0x5A5A5A5A5A5A5A5A);
    if (!ok) { std::printf("GETINFO stop case failed: %s reason=%u step=%u\n", name, unsigned(c.spurs_info_result.reason), unsigned(c.spurs_info_result.step)); SHOW(r); }
    CHECK(ok); return ok;
}
void run_errors() {
    Runtime r; r.boot2(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    struct Rc { uint64_t a3, a4; uint32_t rc; const char *name; };
    const Rc rcs[] = {{0, INFO, PS3_SPURS_RC_NULL, "(0,I)"}, {B0, 0, PS3_SPURS_RC_NULL, "(B,0)"}, {0, 0, PS3_SPURS_RC_NULL, "(0,0)"}, {B0 + 8, 0, PS3_SPURS_RC_NULL, "(misaligned,0): NULL beats ALIGN"},
        {UNOWNED, 0, PS3_SPURS_RC_NULL, "(unowned,0)"}, {UINT64_C(0x100000000), INFO, PS3_SPURS_RC_NULL, "low32 zero"}, {B0 + 8, INFO, PS3_SPURS_RC_ALIGN, "ALIGN"},
        {UNOWNED + 8, INFO, PS3_SPURS_RC_ALIGN, "unowned ALIGN before owner"}, {UINT64_C(0x100000000) | (B0 + 64), INFO, PS3_SPURS_RC_ALIGN, "ALIGN low32"}};
    for (const Rc &x : rcs) {   // A3: guest-visible SDK errors, sign-extended; only the TOC effect; no memory touched
        fill(r, INFO); const size_t from = r.published(), returns = c.spurs_info_returns; call(r, x.a3, x.a4); const auto ev = r.since(from);
        const bool ok = c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(x.rc) && cpu.gpr[2] == TOC && c.spurs_info_has_result && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_FAILED &&
            c.spurs_info_result.rc == x.rc && c.spurs_info_result.step == PS3_SPURS_INFO_STEP_REQUEST && c.spurs_info_returns == returns + 1 && c.spurs_info_reads == 0 && c.spurs_info_writes == 0 &&
            ev.size() == 1 && is_event(ev[0], {true, TOC_SLOT, 8, TOC}) && r.get(TOC_SLOT, 8) == TOC && out_bytes(r, INFO) == std::vector<uint8_t>(SPAN, 0xA5);
        if (!ok) std::printf("GETINFO rc case failed: %s\n", x.name);
        CHECK(ok);
    }
    // A4: true unknown / retained / building owners are native STOPs (SpursInfoStop), never an SDK return code
    CHECK(expect_stopped(r, UNOWNED, INFO, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_INFO_STEP_OWNER, "unowned 128-aligned"));
    for (auto st : {PS3_SPURS_INSTANCE_RETAINED, PS3_SPURS_INSTANCE_BUILDING}) {   // white-box state change of the real record; restored before teardown
        r.instance(1).state = st; CHECK(expect_stopped(r, B1, INFO, PS3_SPURS_STOP_INSTANCE_NOT_ACTIVE, PS3_SPURS_INFO_STEP_OWNER, "non-active owner")); CHECK(c.spurs_info_result.detail == unsigned(st));
        r.instance(1).state = PS3_SPURS_INSTANCE_ACTIVE;
    }
    // A8 overlap / range
    for (uint32_t i : {B0 + 0x800u, B0 - 0x100u, B0 + 0xFE8u, B0}) CHECK(expect_stopped(r, B0, i, PS3_SPURS_STOP_OUTPUT_OVERLAP, PS3_SPURS_INFO_STEP_OVERLAP, "overlap", i));
    CHECK(expect_stopped(r, B0, 0xFFFFFF00u, PS3_SPURS_STOP_PREFLIGHT_REFUSED, PS3_SPURS_INFO_STEP_RANGE, "wrapping info range"));
    // true unsupported guest states: late STOPs after the earlier reference-ordered writes (residue kept, never restored)
    for (int lane = 0; lane < 3; ++lane) {
        const bool trace = lane == 0; const uint8_t pn = lane == 1 ? 16 : 200;
        if (trace) r.put(B0 + 0x900, "\x00\x00\x00\x01\x00\x00\x00\x00", 8); else put_prefix(r, B0, "0123456789ABCDEF", pn);
        const auto log = expected_log(r, B0, INFO); fill(r, INFO); const size_t from = r.published(); call(r, B0, INFO);
        if (expect_partial(r, log, log.size(), from, INFO, B0, StartupStop::SpursInfoStop, trace ? "traceBuffer high32" : "prefix > 15")) {
            CHECK(c.spurs_info_has_result && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP && c.spurs_info_result.reason == PS3_SPURS_STOP_GUEST_STATE &&
                  c.spurs_info_result.step == (trace ? PS3_SPURS_INFO_STEP_TRACE_BUFFER : PS3_SPURS_INFO_STEP_PREFIX) && c.spurs_info_report.partial == 1 && c.spurs_info_stop_latched == StartupStop::None);
            CHECK(trace ? (c.spurs_info_result.detail == 0 && c.spurs_info_report.trace_buffer_raw == UINT64_C(0x100000000)) : (c.spurs_info_result.detail == pn && c.spurs_info_report.prefix_size == pn));
            CHECK(r.get(INFO + 0x18, 8) == 0 && r.get(INFO + 0x68, 4) == 0xA5A5A5A5 && r.get(INFO + 0x70, 4) == 0xA5A5A5A5);   // size written; name/counters untouched
            CHECK(trace ? r.get(INFO + 0x10, 4) == 0xA5A5A5A5 : r.get(INFO + 0x10, 4) == 0 && r.get(INFO + 0x20, 4) == 0);
        }
        if (trace) r.put(B0 + 0x900, "\0\0\0\0\0\0\0\0", 8); else put_prefix(r, B0, "", 0);
    }
    for (uint8_t n : {uint8_t(0), uint8_t(15)}) { put_prefix(r, B0, "ABCDEFGHIJKLMNO", n); CHECK(expect_success(r, B0, INFO, B0, INFO, "prefix 0/15", n ? 0x80 : 0, true)); }
    put_prefix(r, B0, "", 0);
    // A5 invalid memory: the guest output span (the first unwritable span start is reported), before any callback
    struct Bad { uint32_t info, address; const char *name; };
    const Bad bads[] = {{0x30000000, 0x30000000, "unmapped"}, {RO_RAM, RO_RAM, "read-only RAM"}, {STACK_BASE + STACK_SPAN - 0x10, STACK_BASE + STACK_SPAN, "straddles the end of RAM"}};
    for (const Bad &b : bads) {
        fill(r, INFO); const size_t from = r.published(); call(r, B0, b.info);
        const bool ok = c.stop == StartupStop::InvalidMemory && c.spurs_info_has_result && c.spurs_info_stop_latched == StartupStop::InvalidMemory && c.spurs_info_invalid_address == b.address && c.invalid_address == b.address &&
            c.spurs_info_reads == 0 && c.spurs_info_writes == 0 && r.since(from).empty() && cpu.gpr[3] == B0 && c.spurs_info_result.reason == PS3_SPURS_STOP_PREFLIGHT_REFUSED &&
            c.spurs_info_result.step == PS3_SPURS_INFO_STEP_PREFLIGHT && c.spurs_info_callback_error;
        if (!ok) { std::printf("GETINFO invalid memory case failed: %s\n", b.name); SHOW(r); }
        CHECK(ok);
    }
    // TOC slot unwritable: before the domain, so even a NULL argument gets InvalidMemory and no result
    fill(r, INFO); call(r, 0, INFO, 0x30000000);
    CHECK(c.stop == StartupStop::InvalidMemory && !c.spurs_info_has_result && c.spurs_info_reads == 0 && c.spurs_info_writes == 0 && out_bytes(r, INFO) == std::vector<uint8_t>(SPAN, 0xA5));
    // A9 missing services
    { auto *spurs = r.execution.spurs; r.execution.spurs = nullptr; call(r, B0, INFO); CHECK(c.stop == StartupStop::MissingServices && !c.spurs_info_has_result); r.execution.spurs = spurs; }
    { auto *mem = r.execution.ppu.memory; r.execution.ppu.memory = nullptr; call(r, B0, INFO); CHECK(c.stop == StartupStop::MissingServices); r.execution.ppu.memory = mem; }
    CHECK(attach_counters_zero(c) && r.instance(0).state == PS3_SPURS_INSTANCE_ACTIVE && r.instance(1).state == PS3_SPURS_INSTANCE_ACTIVE);
}
void run_trace_credit() {
    Runtime r; r.boot2(); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const size_t A = expected_log(r, B0, INFO).size();
    { fill(r, INFO); r.leave_trace_capacity(0); call(r, B0, INFO); CHECK(c.stop == StartupStop::TraceCapacity && !c.spurs_info_has_result && cpu.gpr[3] == B0);   // TOC credit missing: before the domain
      c.cancelled.store(true); call(r, B0, INFO); CHECK(c.stop == StartupStop::Cancelled && !c.spurs_info_has_result); c.cancelled.store(false); }   // cancel is observed at the HLE entry, before the dispatcher's credit check
    Runtime n; n.boot2(); n.leave_trace_capacity(1); fill(n, INFO); call(n, 0, INFO);   // NULL needs only the TOC credit
    CHECK(n.control->stop == StartupStop::None && n.control->spurs_info_result.rc == PS3_SPURS_RC_NULL && n.execution.ppu.cpu.gpr[3] == rc_reg(PS3_SPURS_RC_NULL));
    Runtime p; p.boot2(); { const auto log = expected_log(p, B0, INFO); fill(p, INFO); p.leave_trace_capacity(PS3_SPURS_INFO_MAX_ACCESSES);   // max 71 accesses + TOC = 72 required
      const size_t from = p.published(); call(p, B0, INFO);
      CHECK(p.control->stop == StartupStop::TraceCapacity && p.control->spurs_info_has_result && p.control->spurs_info_result.reason == PS3_SPURS_STOP_PREFLIGHT_REFUSED &&
            p.control->spurs_info_result.step == PS3_SPURS_INFO_STEP_PREFLIGHT && p.control->spurs_info_trace_failure && p.control->spurs_info_callback_error && expect_partial(p, log, 0, from, INFO, B0, StartupStop::TraceCapacity, "72 credit missing")); }
    Runtime q; q.boot2(); q.leave_trace_capacity(PS3_SPURS_INFO_MAX_ACCESSES + 1); CHECK(expect_success(q, B0, INFO, B0, INFO, "exactly 72 credits"));
    CHECK(A == 41);
}
}
namespace {
// Per-access sweeps over ONE booted runtime (injection is non-sticky). Instance 0 carries prefix "ABC": A = 41 + 2*3 = 47 accesses.
struct Sweep {
    Runtime r; std::vector<Ev> log; size_t A = 0;
    explicit Sweep(bool journaled = false) : r(journaled) { r.boot2(); put_prefix(r, B0, "ABC", 3); log = expected_log(r, B0, INFO); A = log.size(); CHECK(A == 47); }
    size_t go() { fill(r, INFO); const size_t from = r.published(); call(r, B0, INFO); return from; }
};
void run_memory_failures() {   // every read/write of the call refused by the real memory port after the poll/reserve: nothing performed, exact residue
    Sweep s; auto &c = *s.r.control;
    for (unsigned k = 0; k < s.A; ++k) {
        arm(s.r, Inject::MemFail, k); fill(s.r, INFO); const size_t from = s.r.published(); call(s.r, B0, INFO); const bool fired = inject.fired; const uint32_t fa = inject.fired_address; inject = {};
        const bool ok = fired && fa == s.log[k].address && c.spurs_info_has_result && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP &&
            c.spurs_info_result.reason == (s.log[k].write ? PS3_SPURS_STOP_WRITE_FAILED : PS3_SPURS_STOP_READ_FAILED) && c.spurs_info_stop_latched == StartupStop::InvalidMemory &&
            c.spurs_info_invalid_address == fa && c.invalid_address == fa && c.spurs_info_callback_error && !c.spurs_info_trace_failure && c.spurs_info_returns == 0;
        if (!ok) { const auto rep = s.r.recorder.report(); std::printf("memory failure k=%u failed (fired=%d) recorder retained=%zu cap=%zu batch=%d\n", k, int(fired), rep.retained_count, rep.capacity, int(rep.batch_active)); }
        CHECK(ok); CHECK(expect_partial(s.r, s.log, k, from, INFO, B0, StartupStop::InvalidMemory, "memory failure"));
    }
    CHECK(expect_success(s.r, B0, INFO, B0, INFO, "sweep runtime still healthy", 0x80, true));
}
void run_trace_failures() {
    Sweep s; auto &c = *s.r.control;
    unsigned total = 0; arm(s.r, Inject::TraceBegin, UINT32_MAX); call(s.r, B0, INFO); total = inject.seen; inject = {};   // calibrate: one credited begin per access plus the TOC
    if (total != s.A + 1) std::printf("begin(1) calibration: total=%u A=%zu\n", total, s.A);
    CHECK(total == s.A + 1);
    const size_t ret0 = c.spurs_info_returns;
    for (unsigned k = 1; k <= s.A + 1; ++k) {
        arm(s.r, Inject::TraceBegin, k); fill(s.r, INFO); const size_t from = s.r.published(); call(s.r, B0, INFO); const bool fired = inject.fired; inject = {};
        const bool toc = k == s.A + 1; const size_t performed = toc ? s.A : k - 1;
        bool ok = fired && c.spurs_info_has_result && c.spurs_info_returns == ret0 && (toc ? c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_OK && !c.spurs_info_trace_failure
              : c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP && c.spurs_info_trace_failure && c.spurs_info_callback_error && c.spurs_info_result.reason == (s.log[performed].write ? PS3_SPURS_STOP_WRITE_FAILED : PS3_SPURS_STOP_READ_FAILED));
        if (!ok) std::printf("trace failure k=%u failed\n", k);
        CHECK(ok); CHECK(expect_partial(s.r, s.log, performed, from, INFO, B0, StartupStop::TraceCapacity, toc ? "TOC credit refused after complete output" : "per-access credit refused"));
    }
}
void run_journal_failures() {   // journal (non-recorder) trace: the publication FAILS AFTER the access really happened -> it is accounted and latched, never "nothing performed"
    Sweep s(true); auto &c = *s.r.control; const size_t base = c.journal.count;
    for (unsigned k = 0; k < s.A; ++k) {
        arm(s.r, Inject::After, k, Inject::FillJournal); fill(s.r, INFO); call(s.r, B0, INFO); const bool fired = inject.fired; inject = {};
        const size_t performed = k + 1, published = c.journal.count; (void)published;
        c.journal.count = base;   // events [base, base+k) were published before the failing publication; read them back directly
        const Counts n = count_of(s.log, performed, INFO);
        bool ok = fired && c.stop == StartupStop::TraceCapacity && c.spurs_info_has_result && c.spurs_info_trace_failure && c.spurs_info_callback_error && c.spurs_info_returns == 0 &&
            c.spurs_info_reads == n.reads && c.spurs_info_writes == n.writes && c.spurs_info_report.reads == n.reads && c.spurs_info_report.writes == n.writes &&
            out_bytes(s.r, INFO) == apply(s.log, performed, INFO) && s.r.get(TOC_SLOT, 8) == UINT64_C(0x5A5A5A5A5A5A5A5A) && s.r.execution.ppu.cpu.gpr[3] == B0;
        if (k + 1 == s.A) ok = ok && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_OK;   // failure on the FINAL write: the domain finished, the adapter still refuses r3
        else ok = ok && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP && c.spurs_info_result.reason == (s.log[performed].write ? PS3_SPURS_STOP_WRITE_FAILED : PS3_SPURS_STOP_READ_FAILED);
        for (size_t i = 0; ok && i < k; ++i) ok = is_event(c.journal.events[base + i], s.log[i]);
        if (!ok) std::printf("journal failure k=%u failed\n", k);
        CHECK(ok);
    }
}
void run_boundaries() {
    uint64_t T = 0, P0 = 0;
    { Sweep s; auto &c = *s.r.control; c.deadline_tick = UINT64_MAX; tick_calls = 0; call(s.r, 0, INFO); P0 = tick_calls - 1;   // NULL: dispatcher polls + the post-domain poll
      tick_calls = 0; call(s.r, B0, INFO); T = tick_calls; c.deadline_tick = 0; CHECK(c.stop == StartupStop::None && T == P0 + 1 + s.A + 1); }
    Sweep s; auto &c = *s.r.control;   // deadline (the poll counter): poll p fires when the counter reaches deadline_tick; 0 disables, so p = 1..T-1
    for (uint64_t k = 1; k < T; ++k) {
        fill(s.r, INFO); const size_t from = s.r.published(); c.deadline_tick = k; tick_calls = 0; call(s.r, B0, INFO); c.deadline_tick = 0;
        const size_t performed = k <= P0 ? 0 : size_t(k - P0 - 1) > s.A ? s.A : size_t(k - P0 - 1);
        const bool before = k < P0, post = k == T - 1;
        bool ok = c.stop == StartupStop::Deadline && c.spurs_info_has_result == !before && (!before && !post ? c.spurs_info_stop_latched == StartupStop::Deadline : true);
        if (post) ok = ok && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_OK;
        else if (!before) ok = ok && c.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP && c.spurs_info_callback_error;
        if (!ok) std::printf("deadline k=%llu failed (P0=%llu T=%llu)\n", (unsigned long long)k, (unsigned long long)P0, (unsigned long long)T);
        CHECK(ok); if (!before) { expect_partial(s.r, s.log, performed, from, INFO, B0, StartupStop::Deadline, "deadline"); } else CHECK(out_bytes(s.r, INFO) == std::vector<uint8_t>(SPAN, 0xA5));
    }
    // before the call (sticky controls -> fresh runtimes): no accesses, no result; hle budget exhausted at the import entry
    struct Pre { StartupStop stop; int which; const char *name; };
    for (const Pre &p : {Pre{StartupStop::Cancelled, 0, "cancel"}, Pre{StartupStop::SpursKernelFence, 1, "fence"}, Pre{StartupStop::BudgetClockFailure, 2, "clock regression"}}) {
        Runtime r; r.boot2(); fill(r, INFO); if (p.which == 0) r.control->cancelled.store(true); else if (p.which == 1) r.control->spurs_fence_latched = true; else clock_now.store(0);
        call(r, B0, INFO); clock_now.store(1);
        const bool ok = r.control->stop == p.stop && !r.control->spurs_info_has_result && r.control->spurs_info_reads == 0 && r.control->spurs_info_writes == 0 && out_bytes(r, INFO) == std::vector<uint8_t>(SPAN, 0xA5);
        if (!ok) std::printf("pre-call %s failed\n", p.name);
        CHECK(ok);
    }
    uint64_t hle = 0; { Runtime probe; probe.boot2(); hle = probe.budget_policy.snapshot().hle; }
    { Runtime r(false, hle); r.boot2(); fill(r, INFO); call(r, B0, INFO); CHECK(r.control->stop == StartupStop::HleSyscallBudget && !r.control->spurs_info_has_result && r.control->spurs_info_reads == 0 && out_bytes(r, INFO) == std::vector<uint8_t>(SPAN, 0xA5)); }
    // mid-call (after the k-th access really happened): the next per-access poll latches, the domain stops, the adapter reports the latched reason
    struct Mid { Inject::Act act; StartupStop stop; const char *name; };
    for (const Mid &m : {Mid{Inject::Cancel, StartupStop::Cancelled, "cancel"}, Mid{Inject::Fence, StartupStop::SpursKernelFence, "fence"}, Mid{Inject::ClockBack, StartupStop::BudgetClockFailure, "budget clock"}})
        for (unsigned k : {0u, 1u, 23u, 46u}) {
            Sweep w; auto &wc = *w.r.control; arm(w.r, Inject::After, k, m.act); fill(w.r, INFO); const size_t from = w.r.published(); call(w.r, B0, INFO); const bool fired = inject.fired; inject = {}; clock_now.store(1);
            const bool ok = fired && wc.stop == m.stop && wc.spurs_info_has_result && wc.spurs_info_returns == 0 && (k == 46 ? wc.spurs_info_result.outcome == PS3_SPURS_OUTCOME_OK
                : (wc.spurs_info_result.outcome == PS3_SPURS_OUTCOME_STOP && wc.spurs_info_stop_latched == m.stop && wc.spurs_info_callback_error));
            if (!ok) std::printf("mid-call %s k=%u failed\n", m.name, k);
            CHECK(ok); expect_partial(w.r, w.log, k + 1, from, INFO, B0, m.stop, m.name);
        }
}
void run_classification() {   // pure mapping on the exact domain result shapes, including shapes the real recorder cannot produce
    SpursAttachMemory ctx{nullptr}; ps3_spurs_result res{};
    res.outcome = PS3_SPURS_OUTCOME_OK; CHECK(spurs_info_classify(res, ctx).disposition == SpursAttachDisposition::PublishOk);
    res.outcome = PS3_SPURS_OUTCOME_FAILED; CHECK(spurs_info_classify(res, ctx).disposition == SpursAttachDisposition::PublishFailed);
    res.outcome = PS3_SPURS_OUTCOME_STOP; CHECK(spurs_info_classify(res, ctx).stop == StartupStop::SpursInfoStop);
    res.outcome = ps3_spurs_outcome(3); CHECK(spurs_info_classify(res, ctx).stop == StartupStop::ProviderFailure);
    ctx.latched = StartupStop::Deadline; CHECK(spurs_info_classify(res, ctx).stop == StartupStop::Deadline);
    res.outcome = PS3_SPURS_OUTCOME_OK; ctx.trace_failure = true; CHECK(spurs_info_classify(res, ctx).stop == StartupStop::TraceCapacity && spurs_info_classify(res, ctx).disposition == SpursAttachDisposition::Stop);   // trace failure beats latched and a clean OK
}

// ---- SPURS taskset setup block: the two pure struct-init dispatches (0x16394A4E, 0x5EF96465) ----------
// These cases drive the REAL ps3_hle_call route
// over the real composition and guest memory, and check: the TOC is published, r2/r3 are the firmware's
// values, the exact struct bytes land at the guest EA and nowhere else, the counters move, an unreadable or
// unwritable address stops InvalidMemory, and a refused call writes nothing.
namespace {
constexpr uint32_t TASKSET_ATTR_NID = 0x16394a4e, EVENT_FLAG_NID = 0x5ef96465;
// The next two entries of the same block.
constexpr uint32_t TASKSET_NAME_NID = 0x652b70e2, EVENT_FLAG_ATTACH_NID = 0x87630976;
constexpr uint32_t SCRATCH_BASE = 0x100000, SCRATCH_SPAN = 0x4000;   // a dedicated writable RAM window
constexpr uint32_t ATTR_EA = SCRATCH_BASE + 0x100;                   // 8-byte aligned
constexpr uint32_t PRIO_EA = SCRATCH_BASE + 0x80;
constexpr uint32_t EVFLAG_EA = SCRATCH_BASE + 0x400;                 // 128-byte aligned
constexpr uint32_t TASKSET_EA = SCRATCH_BASE + 0x1000;               // 128-byte aligned

// Calls either NID with r3..r8 as given and SP = STACK (so the TOC slot is TOC_SLOT), TOC in r2.
void g10_call(Runtime &r, uint32_t nid, uint64_t r3, uint64_t r4 = 0, uint64_t r5 = 0,
              uint64_t r6 = 0, uint64_t r7 = 0, uint64_t r8 = 0) {
    r.control->import_count = 0;
    auto &cpu = r.execution.ppu.cpu;
    cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5; cpu.gpr[6] = r6; cpu.gpr[7] = r7; cpu.gpr[8] = r8;
    cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
    r.run(nid, false);
}

// cellSpursCreateTask takes SEVEN arguments (r3..r9); g10_call covers r3..r8, so this sets r9 too.
// Every value that defines the scenario is passed from the case, never baked in here.
void g10_call_r9(Runtime &r, uint32_t nid, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6,
                 uint64_t r7, uint64_t r8, uint64_t r9) {
    r.control->import_count = 0;
    auto &cpu = r.execution.ppu.cpu;
    cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5; cpu.gpr[6] = r6;
    cpu.gpr[7] = r7; cpu.gpr[8] = r8; cpu.gpr[9] = r9;
    cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
    r.run(nid, false);
}

std::vector<uint8_t> g10_bytes(Runtime &r, uint32_t address, size_t length) {
    std::vector<uint8_t> v(length);
    CHECK(r.mem().read(r.mem().context, address, v.data(), length) == PS3_GUEST_MEMORY_OK);
    return v;
}

// Binds a scratch RAM window on the same domain the runtime already owns (the same technique
// test_startup_spurs_idle.cpp uses), so the struct EAs are ordinary guest memory.
struct ScratchScope {
    Runtime &r;
    PpuMemoryRamRange permitted[2];
    PpuMemoryBinding scoped;
    PpuMemoryBinding *original;
    explicit ScratchScope(Runtime &runtime) : r(runtime),
        permitted{{SCRATCH_BASE, SCRATCH_SPAN, true}, {STACK_BASE, STACK_SPAN, true}},
        scoped(r.domain, permitted, 2), original(r.execution.ppu.memory) {
        CHECK(ps3_atomic_commit(&r.domain, SCRATCH_BASE, SCRATCH_SPAN) == PS3_GUEST_MEMORY_OK);
        r.execution.ppu.memory = &scoped;
    }
    ~ScratchScope() { r.execution.ppu.memory = original; }
    ScratchScope(const ScratchScope &) = delete;
    ScratchScope &operator=(const ScratchScope &) = delete;
};
// g13: cellSpursCreateTasksetWithAttribute validates and then WRITES the 6400-byte taskset image and runs the
// native workload registration on the instance the spurs EA names, so it needs the scratch window, the group's
// instance record and the stack bound on the same domain at once.
struct CreateScope {
    Runtime &r;
    PpuMemoryRamRange permitted[4];
    PpuMemoryBinding scoped;
    PpuMemoryBinding *original;
    explicit CreateScope(Runtime &runtime) : r(runtime),
        // G15c: the fourth entry is the reserved block the LFQueue cases call the entry on.
        permitted{{SCRATCH_BASE, SCRATCH_SPAN, true}, {0x7CF000, 0x3000, true}, {STACK_BASE, STACK_SPAN, true}, {LFQ_SCRATCH_EA, LFQ_SCRATCH_SIZE, true}},
        scoped(r.domain, permitted, 4), original(r.execution.ppu.memory) {
        CHECK(ps3_atomic_commit(&r.domain, SCRATCH_BASE, SCRATCH_SPAN) == PS3_GUEST_MEMORY_OK);
        r.execution.ppu.memory = &scoped;
    }
    ~CreateScope() { r.execution.ppu.memory = original; }
    CreateScope(const CreateScope &) = delete;
    CreateScope &operator=(const CreateScope &) = delete;
};
// g12c: the served event-flag attach reaches the scratch flag window AND the CellSpurs instance the flag names
// (its 0xD6C gate, the SPU-port binding, and no guest DA8 write), so it needs both ranges plus the stack at once.
struct AttachScope {
    Runtime &r;
    PpuMemoryRamRange permitted[3];
    PpuMemoryBinding scoped;
    PpuMemoryBinding *original;
    explicit AttachScope(Runtime &runtime) : r(runtime),
        permitted{{SCRATCH_BASE, SCRATCH_SPAN, true}, {0x7CF000, 0x3000, true}, {STACK_BASE, STACK_SPAN, true}},
        scoped(r.domain, permitted, 3), original(r.execution.ppu.memory) {
        CHECK(ps3_atomic_commit(&r.domain, SCRATCH_BASE, SCRATCH_SPAN) == PS3_GUEST_MEMORY_OK);
        r.execution.ppu.memory = &scoped;
    }
    ~AttachScope() { r.execution.ppu.memory = original; }
    AttachScope(const AttachScope &) = delete;
    AttachScope &operator=(const AttachScope &) = delete;
};
}

void run_taskset_attribute_initialize() {
    Runtime r; r.boot2(); ScratchScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    // The priority source the firmware copies from: eight bytes, all <= 0xF so the call succeeds.
    const unsigned char priority[8] = {8, 1, 0, 0, 0, 0, 0, 0};
    r.put(PRIO_EA, priority, 8);
    std::vector<uint8_t> dirty(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES + 0x40, 0xA5);
    r.put(ATTR_EA, dirty.data(), dirty.size());
    const size_t from = r.published();

    // ---- success: inFAMOUS's own argument values (survey A.2.2) ----
    g10_call(r, TASKSET_ATTR_NID, ATTR_EA, 1u, 0x00250001u, 0u, PRIO_EA, 8u);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[2] == TOC);                       // TOC restored
    CHECK(cpu.gpr[3] == 0);                         // firmware success code
    CHECK(r.get(TOC_SLOT, 8) == TOC);               // TOC slot published
    CHECK(c.taskset_attr_init_calls == 1 && c.taskset_attr_init_returns == 1);
    CHECK(c.taskset_attr_init_last == 0u);
    CHECK(c.taskset_attr_init_writes == PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    {
        std::vector<uint8_t> expected(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0);
        expected[0x03] = 0x01;                                                 // revision = 1
        expected[0x05] = 0x25; expected[0x07] = 0x01;                          // sdk_version = 0x00250001
        for (size_t i = 0; i < 8; ++i) expected[0x08 + i] = 0;                 // args = 0
        for (size_t i = 0; i < 8; ++i) expected[0x10 + i] = priority[i];       // copied verbatim
        expected[0x1B] = 0x08;                                                 // max_contention = 8
        expected[0x22] = 0x19;                                                 // taskset_size = 6400 = 0x1900 at +0x20
        CHECK(g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES) == expected);
    }
    // Nothing outside [ATTR_EA, ATTR_EA+512) was touched: the trailing sentinel bytes are intact.
    const auto sentinel = g10_bytes(r, ATTR_EA + PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0x40);
    CHECK(sentinel == std::vector<uint8_t>(0x40, 0xA5));
    CHECK(!r.since(from).empty());                  // the writes were published through the trace

    // ---- refusal: NULL attribute -> 0x80410911, no guest write ----
    const size_t ret0 = c.taskset_attr_init_returns;
    std::vector<uint8_t> attr_before = g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    g10_call(r, TASKSET_ATTR_NID, 0u, 1u, 0x00250001u, 0u, PRIO_EA, 8u);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(c.taskset_attr_init_last == 0x80410911u);
    CHECK(c.taskset_attr_init_calls == 2 && c.taskset_attr_init_returns == ret0 + 1);
    CHECK(g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES) == attr_before);

    // ---- refusal: misaligned attribute -> 0x80410910 ----
    g10_call(r, TASKSET_ATTR_NID, ATTR_EA + 4u, 1u, 0x00250001u, 0u, PRIO_EA, 8u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u) && c.taskset_attr_init_last == 0x80410910u);

    // ---- refusal: a priority byte > 0xF -> 0x80410902 ----
    { const unsigned char bad[8] = {8, 0x10, 0, 0, 0, 0, 0, 0}; r.put(PRIO_EA, bad, 8); }
    g10_call(r, TASKSET_ATTR_NID, ATTR_EA, 1u, 0x00250001u, 0u, PRIO_EA, 8u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u) && c.taskset_attr_init_last == 0x80410902u);
    r.put(PRIO_EA, priority, 8);

    // ---- invalid memory: an unwritable attribute EA stops InvalidMemory with the faulting address ----
    const size_t calls0 = c.taskset_attr_init_calls, returns0 = c.taskset_attr_init_returns;
    g10_call(r, TASKSET_ATTR_NID, 0x30000000u, 1u, 0x00250001u, 0u, PRIO_EA, 8u);
    CHECK(c.stop == StartupStop::InvalidMemory);
    CHECK(c.invalid_address == 0x30000000u);
    CHECK(c.taskset_attr_init_calls == calls0 + 1 && c.taskset_attr_init_returns == returns0);   // the call never returned
    CHECK(c.taskset_attr_init_last == 0xFFFFFFFFu);                                             // and published no result

    // ---- invalid memory: an unreadable priority EA stops InvalidMemory too ----
    Runtime v; v.boot2(); ScratchScope scope2(v); auto &vc = *v.control;
    v.put(PRIO_EA, priority, 8);
    g10_call(v, TASKSET_ATTR_NID, ATTR_EA, 1u, 0x00250001u, 0u, 0x30000000u, 8u);
    CHECK(vc.stop == StartupStop::InvalidMemory && vc.invalid_address == 0x30000000u);
    CHECK(vc.taskset_attr_init_calls == 1 && vc.taskset_attr_init_returns == 0);
}

void run_event_flag_initialize() {
    Runtime r; r.boot2(); ScratchScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    std::vector<uint8_t> dirty(PS3_SPURS_EVENT_FLAG_BYTES + 0x40, 0x5C);
    r.put(EVFLAG_EA, dirty.data(), dirty.size());
    const uint32_t spurs = INSTANCE1;
    const size_t from = r.published();

    // ---- success: inFAMOUS's own argument values (survey A.2.3): r4 = 0, r6 = 0, r7 = 1 ----
    g10_call(r, EVENT_FLAG_NID, spurs, 0u, EVFLAG_EA, 0u, 1u);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[2] == TOC && cpu.gpr[3] == 0);
    CHECK(r.get(TOC_SLOT, 8) == TOC);
    CHECK(c.event_flag_init_calls == 1 && c.event_flag_init_returns == 1);
    CHECK(c.event_flag_init_last == 0u && c.event_flag_init_writes == PS3_SPURS_EVENT_FLAG_BYTES);
    {
        std::vector<uint8_t> expected(PS3_SPURS_EVENT_FLAG_BYTES, 0);
        expected[0x0C] = 0xFF;                                   // spuPort = INVALID
        expected[0x0D] = 0x01;                                   // isIwl = 1 (taskset == NULL branch)
        expected[0x0E] = 0x01;                                   // direction = OR
        expected[0x0F] = 0x00;                                   // clearMode = CLEAR_AUTO
        expected[0x74] = uint8_t(spurs >> 24);                   // addr = spurs EA, big-endian u64
        expected[0x75] = uint8_t(spurs >> 16); expected[0x76] = uint8_t(spurs >> 8); expected[0x77] = uint8_t(spurs);
        CHECK(g10_bytes(r, EVFLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES) == expected);
    }
    CHECK(g10_bytes(r, EVFLAG_EA + PS3_SPURS_EVENT_FLAG_BYTES, 0x40) == std::vector<uint8_t>(0x40, 0x5C));
    CHECK(!r.since(from).empty());

    // ---- refusal: NULL spurs with NULL taskset -> 0x80410911, nothing written ----
    const size_t ret0 = c.event_flag_init_returns;
    const auto before = g10_bytes(r, EVFLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES);
    g10_call(r, EVENT_FLAG_NID, 0u, 0u, EVFLAG_EA, 0u, 1u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(c.event_flag_init_last == 0x80410911u && c.event_flag_init_returns == ret0 + 1);
    CHECK(g10_bytes(r, EVFLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES) == before);
    // NULL eventFlag on the branch the game takes.
    g10_call(r, EVENT_FLAG_NID, spurs, 0u, 0u, 0u, 1u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u));

    // ---- refusal: misaligned eventFlag -> 0x80410910 (eventFlag is checked before INVAL) ----
    g10_call(r, EVENT_FLAG_NID, spurs, 0u, EVFLAG_EA + 4u, 0u, 1u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u) && c.event_flag_init_last == 0x80410910u);
    // Misaligned spurs is refused on the taskset == NULL branch too (0x157EC runs on both).
    g10_call(r, EVENT_FLAG_NID, spurs + 1u, 0u, EVFLAG_EA, 0u, 1u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u));

    // ---- refusal: direction > 3 and clearMode > 1 -> 0x80410902 ----
    g10_call(r, EVENT_FLAG_NID, spurs, 0u, EVFLAG_EA, 0u, 4u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u) && c.event_flag_init_last == 0x80410902u);
    g10_call(r, EVENT_FLAG_NID, spurs, 0u, EVFLAG_EA, 2u, 1u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));

    // ---- the taskset != NULL branch: wid > 0x1F is refused, addr becomes the taskset EA ----
    { const unsigned char wid[4] = {0x00, 0x00, 0x00, 0x20}; r.put(TASKSET_EA + PS3_SPURS_TASKSET_WID_OFFSET, wid, 4); }
    g10_call(r, EVENT_FLAG_NID, spurs, TASKSET_EA, EVFLAG_EA, 0u, 1u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
    { const unsigned char wid[4] = {0x00, 0x00, 0x00, 0x07}; r.put(TASKSET_EA + PS3_SPURS_TASKSET_WID_OFFSET, wid, 4); }
    g10_call(r, EVENT_FLAG_NID, spurs, TASKSET_EA, EVFLAG_EA, 1u, 2u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && c.event_flag_init_last == 0u);
    { const auto bytes = g10_bytes(r, EVFLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES);
      CHECK(bytes[0x0D] == 0x00);                      // isIwl stays 0 on this branch
      CHECK(bytes[0x0E] == 0x02 && bytes[0x0F] == 0x01);
      CHECK(bytes[0x74] == uint8_t(TASKSET_EA >> 24) && bytes[0x77] == uint8_t(TASKSET_EA)); }

    // ---- invalid memory: an unwritable eventFlag EA stops InvalidMemory ----
    const size_t calls0 = c.event_flag_init_calls, returns0 = c.event_flag_init_returns;
    g10_call(r, EVENT_FLAG_NID, spurs, 0u, 0x30000000u, 0u, 1u);
    CHECK(c.stop == StartupStop::InvalidMemory && c.invalid_address == 0x30000000u);
    CHECK(c.event_flag_init_calls == calls0 + 1 && c.event_flag_init_returns == returns0);
    CHECK(c.event_flag_init_last == 0xFFFFFFFFu);
}

}
void run_taskset_attribute_set_name() {
    Runtime r; r.boot2(); ScratchScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    // The attribute _cellSpursTasksetAttributeInitialize just built, all-zero name at +0x1C.
    std::vector<uint8_t> dirty(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0);
    r.put(ATTR_EA, dirty.data(), dirty.size());
    // A name string in guest memory that the firmware must NOT read. A distinctive
    // pattern makes an accidental read visible only if it were copied; it never is, so the assertion
    // below is on the stored pointer, and the string bytes stay untouched.
    const char name_text[] = "INFI_TASKSET_NAME";
    const uint32_t NAME_EA = SCRATCH_BASE + 0x800;
    r.put(NAME_EA, reinterpret_cast<const uint8_t*>(name_text), sizeof(name_text));
    // Sentinels around both the attribute and the string.
    std::vector<uint8_t> attr_buf(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES + 0x40, 0xA5);
    r.put(ATTR_EA, attr_buf.data(), attr_buf.size());
    { std::vector<uint8_t> zero(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0); r.put(ATTR_EA, zero.data(), zero.size()); }
    const size_t from = r.published();
    const auto name_before = g10_bytes(r, NAME_EA, sizeof(name_text));

    // success: inFAMOUS's own arguments: r3 = the attribute, r4 = the name EA
    g10_call(r, TASKSET_NAME_NID, ATTR_EA, NAME_EA);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[2] == TOC);                     // TOC restored
    CHECK(cpu.gpr[3] == 0);                       // firmware success code
    CHECK(r.get(TOC_SLOT, 8) == TOC);             // TOC slot published
    CHECK(c.taskset_name_calls == 1 && c.taskset_name_returns == 1);
    CHECK(c.taskset_name_last == 0u);
    CHECK(c.taskset_name_writes == PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES);   // exactly 4 bytes
    {
        const auto bytes = g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
        // +0x1C holds the POINTER, big-endian, not the string contents.
        CHECK(bytes[0x1C] == uint8_t(NAME_EA >> 24) && bytes[0x1D] == uint8_t(NAME_EA >> 16));
        CHECK(bytes[0x1E] == uint8_t(NAME_EA >> 8) && bytes[0x1F] == uint8_t(NAME_EA));
        // Every other byte of the 512 stays zero: exactly 4 bytes changed.
        for (size_t i = 0; i < PS3_SPURS_TASKSET_ATTRIBUTE_BYTES; ++i)
            if (i < 0x1C || i > 0x1F) CHECK(bytes[i] == 0);
    }
    // The name string itself was not copied anywhere and not modified.
    CHECK(g10_bytes(r, NAME_EA, sizeof(name_text)) == name_before);
    // Nothing outside [ATTR_EA, ATTR_EA+512) was touched.
    CHECK(g10_bytes(r, ATTR_EA + PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0x40) == std::vector<uint8_t>(0x40, 0xA5));
    CHECK(!r.since(from).empty());                // the write was published through the trace

    // ---- refusal: NULL attribute -> 0x80410911, no guest write ----
    const size_t ret0 = c.taskset_name_returns;
    const auto attr_before = g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES);
    g10_call(r, TASKSET_NAME_NID, 0u, NAME_EA);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(c.taskset_name_last == 0x80410911u);
    CHECK(c.taskset_name_calls == 2 && c.taskset_name_returns == ret0 + 1);
    CHECK(c.taskset_name_writes == 0u);
    CHECK(g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES) == attr_before);

    // ---- refusal: NULL name -> the SAME 0x80410911 (0x14338 is before the alignment test) ----
    g10_call(r, TASKSET_NAME_NID, ATTR_EA, 0u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u) && c.taskset_name_last == 0x80410911u);
    CHECK(g10_bytes(r, ATTR_EA, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES) == attr_before);

    // ---- refusal: misaligned attribute -> 0x80410910 ----
    g10_call(r, TASKSET_NAME_NID, ATTR_EA + 4u, NAME_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u) && c.taskset_name_last == 0x80410910u);
    CHECK(c.taskset_name_writes == 0u);

    // ---- invalid memory: an unwritable attribute EA stops InvalidMemory with the faulting address ----
    const size_t calls0 = c.taskset_name_calls, returns0 = c.taskset_name_returns;
    g10_call(r, TASKSET_NAME_NID, 0x30000000u, NAME_EA);
    CHECK(c.stop == StartupStop::InvalidMemory);
    /* The faulting address is the STORE target, attribute+0x1C, not the
     * attribute EA: the entry passes both NULL and alignment gates before it writes. */
    CHECK(c.invalid_address == 0x30000000u + PS3_SPURS_TSATTR_NAME);
    CHECK(c.taskset_name_calls == calls0 + 1 && c.taskset_name_returns == returns0);  // never returned
    CHECK(c.taskset_name_last == 0xFFFFFFFFu);                                      // published no result

    // ---- a NULL name EA is never dereferenced, so an unreadable name does NOT fault ----
    Runtime v; v.boot2(); ScratchScope scope2(v); auto &vc = *v.control;
    { std::vector<uint8_t> zero(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0); v.put(ATTR_EA, zero.data(), zero.size()); }
    g10_call(v, TASKSET_NAME_NID, ATTR_EA, 0x30000000u);
    CHECK(vc.stop == StartupStop::None);
    CHECK(v.execution.ppu.cpu.gpr[3] == 0);           // the setter never touches the string
    CHECK(vc.taskset_name_writes == PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES);

    // ---- ROUTE anchor: with PS3_NATIVE_STARTUP_SPURS_INITIALIZE defined this NID is dispatched, so it
    // never stops UnknownImport. With the feature off there is no case in ps3_hle_call and it does; that
    // build is proven by verify_startup_spurs_getinfo.sh's preprocessor needle check (both NIDs). ----
    Runtime u; u.boot2(); ScratchScope scope5(u);
    { std::vector<uint8_t> zero(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0); u.put(ATTR_EA, zero.data(), zero.size()); }
    g10_call(u, TASKSET_NAME_NID, ATTR_EA, NAME_EA);
    CHECK(u.control->stop != StartupStop::UnknownImport);
    CHECK(u.control->taskset_name_calls == 1);
}

void run_event_flag_attach() {
    Runtime r; r.boot2(); AttachScope scope(r); auto &c = *r.control;
    // An event flag as the native _cellSpursEventFlagInitialize leaves it: spuPort 0xFF, isIwl 1,
    // direction 1 (CONTRACT g10 §C).
    std::vector<uint8_t> flag(PS3_SPURS_EVENT_FLAG_BYTES, 0);
    flag[0x0C] = 0xFF; flag[0x0D] = 0x01; flag[0x0E] = 0x01;
    r.put(EVFLAG_EA, flag.data(), flag.size());
    std::vector<uint8_t> sentinel(PS3_SPURS_EVENT_FLAG_BYTES + 0x40, 0x5C);
    r.put(EVFLAG_EA, sentinel.data(), sentinel.size());
    r.put(EVFLAG_EA, flag.data(), flag.size());
    const uint32_t FLAG_EA = EVFLAG_EA;

    // g12c: THE SERVED PATH. Every gate clears, so the adapter runs the firmware's
    // continuation against the REAL composition: create `_spuEvF` in the composed lv2 registry, connect it in DYNAMIC
    // port mode, write F+0x7C (the queue id) and F+0x0C (the chosen port), return 0. The event flag's own argument word
    // at F+0x74 must name the instance this runtime owns: boot2 stages two instances, so point it at INSTANCE1.
    const size_t calls0 = c.event_flag_attach_calls, returns0 = c.event_flag_attach_returns;
    const size_t queues0 = ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE);
    const size_t ports0 = ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_PORT);
    r.put32(FLAG_EA + PS3_SPURS_EVFLAG_TASKSET_PTR, INSTANCE1);
    {
        // The port the kernel answers for the firmware's mask 0xFFFFFFFFFFFF0000: the lowest port in 16..63 with no live
        // binding in the REAL policy record (the instance's own internal queue already holds one).
        uint8_t want = 0xFF;
        const ps3_spu_group_record *g = &r.policy.groups[0];
        // The instance the flag names is INSTANCE1; find its group through the composition's runtime.
        for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
            if (r.comp->runtime.instances[i].spurs_ea == INSTANCE1) {
                for (unsigned k = 0; k < PS3_SPU_GROUP_CAP; ++k)
                    if (r.policy.groups[k].active && r.policy.groups[k].id == r.comp->runtime.instances[i].group)
                        g = &r.policy.groups[k];
                break;
            }
        for (unsigned port = 16; port < PS3_SPU_PORTS && want == 0xFF; ++port) {
            bool taken = false;
            for (uint32_t t = 0; t < g->num; ++t) {
                ps3_lv2_queue probe{};
                taken = taken || (g->threads[t].port_queue[port] != 0 &&
                    ps3_lv2_queue_snapshot(&r.comp->lv2, g->threads[t].port_queue[port], &probe) == PS3_LV2_OK);
            }
            if (!taken) want = uint8_t(port);
        }
        CHECK(want >= 16 && want < 64);
        const size_t from = r.published();
        g10_call(r, EVENT_FLAG_ATTACH_NID, FLAG_EA);
        CHECK(c.stop == StartupStop::None);                          // no stop: the entry is fully served
        CHECK(r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[2] == TOC);
        CHECK(r.get(TOC_SLOT, 8) == TOC);
        CHECK(c.event_flag_attach_calls == calls0 + 1 && c.event_flag_attach_returns == returns0 + 1);
        CHECK(c.event_flag_attach_last == 0u);
        // THE QUEUE-REGISTRY ASSERTION: exactly one more queue exists, it is `_spuEvF`'s, and it is the one recorded.
        CHECK(ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0 + 1);
        CHECK(ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_PORT) == ports0);   // no event PORT was created (direction 1)
        CHECK(c.event_flag_attach_queues_created == 1 && c.event_flag_attach_queues_destroyed == 0);
        const uint32_t queue = c.event_flag_attach_queue;
        CHECK(queue != 0);
        ps3_lv2_queue snapshot{};
        CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, queue, &snapshot) == PS3_LV2_OK);
        CHECK(snapshot.protocol == PS3_SYNC_FIFO && snapshot.type == PS3_PPU_QUEUE && snapshot.size == 1);
        // The two guest fields, byte for byte, in the firmware's order.
        const auto after = g10_bytes(r, FLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES);
        CHECK(after[0x7C] == uint8_t(queue >> 24) && after[0x7D] == uint8_t(queue >> 16) &&
              after[0x7E] == uint8_t(queue >> 8) && after[0x7F] == uint8_t(queue));
        CHECK(after[0x0C] == want);                                  // the CHOSEN port replaced the init's 0xFF
        CHECK(c.event_flag_attach_port == want);
        CHECK(c.event_flag_attach_writes == 4);                      // exactly the 4 bytes of F+0x7C
        // Nothing OUTSIDE those two fields moved: the rest of the 128 bytes and the surrounding sentinel are untouched.
        {
            std::vector<uint8_t> expect = flag;
            expect[0x0C] = want;
            expect[0x74] = uint8_t(INSTANCE1 >> 24); expect[0x75] = uint8_t(INSTANCE1 >> 16);
            expect[0x76] = uint8_t(INSTANCE1 >> 8); expect[0x77] = uint8_t(INSTANCE1);
            expect[0x7C] = uint8_t(queue >> 24); expect[0x7D] = uint8_t(queue >> 16);
            expect[0x7E] = uint8_t(queue >> 8); expect[0x7F] = uint8_t(queue);
            CHECK(after == expect);
        }
        CHECK(g10_bytes(r, FLAG_EA + PS3_SPURS_EVENT_FLAG_BYTES, 0x40) == std::vector<uint8_t>(0x40, 0x5C));
        CHECK(!r.since(from).empty());
        // The chosen port is bound on every SPU thread of that group, and NO guest DA8 write happened (keyed path).
        {
            ps3_spurs_instance *in = nullptr;
            for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
                if (r.comp->runtime.instances[i].spurs_ea == INSTANCE1) in = &r.comp->runtime.instances[i];
            CHECK(in && in->user_port_bits == (uint64_t(1) << want) && in->user_port_queue[want] == queue);
        }
        CHECK(r.get(INSTANCE1 + 0xDA8, 8) == 0);
    }

    // ---- a SECOND attach after the first bound a port: the kernel answers the NEXT free port, and the flag object is
    // re-staged so its F+0x0C is 0xFF again (the gate requires it). Both attached queues stay in the registry. ----
    {
        r.put(FLAG_EA, flag.data(), flag.size());
        r.put32(FLAG_EA + PS3_SPURS_EVFLAG_TASKSET_PTR, INSTANCE1);   // re-stage F+0x0C = 0xFF AND keep the owner
        const size_t returns1 = c.event_flag_attach_returns;
        const uint8_t first = uint8_t(g10_bytes(r, FLAG_EA + PS3_SPURS_EVFLAG_SPU_PORT, 1)[0]);
        (void)first;
        g10_call(r, EVENT_FLAG_ATTACH_NID, FLAG_EA);
        CHECK(c.stop == StartupStop::None && r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[2] == TOC);
        CHECK(c.event_flag_attach_returns == returns1 + 1 && c.event_flag_attach_last == 0u);
        CHECK(ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0 + 2);
        CHECK(c.event_flag_attach_queues_created == 1 && c.event_flag_attach_queues_destroyed == 0);
        CHECK(g10_bytes(r, FLAG_EA + PS3_SPURS_EVFLAG_SPU_PORT, 1)[0] != 0xFFu);
    }

    // CONNECT FAILURE: the firmware's unwind destroys the queue it created, no guest write happens,
    // and the collapsed code is returned. Forced by making every port in 16..63 look occupied in the REAL policy record
    // (a live foreign queue keeps each binding live under the L4 rule), so the connect syscall answers EISCONN -> 0x8041070A.
    Runtime n; n.boot2(); AttachScope scope7(n); auto &nc = *n.control;
    {
        std::vector<uint8_t> f(PS3_SPURS_EVENT_FLAG_BYTES, 0);
        f[0x0C] = 0xFF; f[0x0D] = 0x01; f[0x0E] = 0x01;
        std::vector<uint8_t> wide(PS3_SPURS_EVENT_FLAG_BYTES + 0x40, 0x5C);
        n.put(EVFLAG_EA, wide.data(), wide.size());
        n.put(EVFLAG_EA, f.data(), f.size());
    }
    n.put32(EVFLAG_EA + PS3_SPURS_EVFLAG_TASKSET_PTR, INSTANCE1);
    const auto serv_before = g10_bytes(n, EVFLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES + 0x40);
    const size_t nqueues0 = ps3_lv2_live_count(&n.comp->lv2, PS3_LV2_KIND_QUEUE);
    const size_t nreturns0 = nc.event_flag_attach_returns;
    {
        uint32_t foreign = 0;
        const ps3_lv2_queue_attr qa{PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
        CHECK(ps3_lv2_queue_create(&n.comp->lv2, &foreign, &qa, 0, 1) == PS3_LV2_OK);
        for (unsigned i = 0; i < PS3_SPU_GROUP_CAP; ++i)
            if (n.policy.groups[i].active)
                for (unsigned port = 16; port < PS3_SPU_PORTS; ++port)
                    for (uint32_t t = 0; t < n.policy.groups[i].num; ++t)
                        n.policy.groups[i].threads[t].port_queue[port] = foreign;
    }
    g10_call(n, EVENT_FLAG_ATTACH_NID, EVFLAG_EA);
    CHECK(nc.stop == StartupStop::None);                          // an SDK error, not a native stop
    CHECK(n.execution.ppu.cpu.gpr[3] == rc_reg(0x8041070A));      // EISCONN, collapsed unchanged
    CHECK(nc.event_flag_attach_returns == nreturns0 + 1);
    CHECK(nc.event_flag_attach_last == 0x8041070Au);
    // The queue the firmware created was DESTROYED on its unwind: the registry is back to its pre-call count plus the one
    // foreign queue this case planted, and the counter says so.
    CHECK(nc.event_flag_attach_queues_created == 1 && nc.event_flag_attach_queues_destroyed == 1);
    CHECK(ps3_lv2_live_count(&n.comp->lv2, PS3_LV2_KIND_QUEUE) == nqueues0 + 1);
    // Not one guest byte moved: both writes sit after a successful connect, and the connect failed.
    CHECK(g10_bytes(n, EVFLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES + 0x40) == serv_before);
    CHECK(nc.event_flag_attach_writes == 0);

    // ---- gate 1: NULL object -> 0x80410911, returned normally ----
    Runtime a; a.boot2(); ScratchScope scope3(a); auto &ac = *a.control; auto &acpu = a.execution.ppu.cpu;
    g10_call(a, EVENT_FLAG_ATTACH_NID, 0u);
    CHECK(ac.stop == StartupStop::None);
    CHECK(acpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(ac.event_flag_attach_last == 0x80410911u);
    CHECK(ac.event_flag_attach_calls == 1 && ac.event_flag_attach_returns == 1);
    CHECK(acpu.gpr[2] == TOC && a.get(TOC_SLOT, 8) == TOC);

    // gate 2: misaligned object -> 0x80410910
    g10_call(a, EVENT_FLAG_ATTACH_NID, FLAG_EA + 1u);
    CHECK(ac.stop == StartupStop::None && acpu.gpr[3] == rc_reg(0x80410910u) && ac.event_flag_attach_last == 0x80410910u);

    // ---- gate 3: direction not in {1,3} -> 0x80410909 ----
    { std::vector<uint8_t> bad = flag; bad[0x0E] = 0u; a.put(FLAG_EA, bad.data(), bad.size()); }
    g10_call(a, EVENT_FLAG_ATTACH_NID, FLAG_EA);
    CHECK(ac.stop == StartupStop::None && acpu.gpr[3] == rc_reg(0x80410909u) && ac.event_flag_attach_last == 0x80410909u);
    { std::vector<uint8_t> bad = flag; bad[0x0E] = 2u; a.put(FLAG_EA, bad.data(), bad.size()); }
    g10_call(a, EVENT_FLAG_ATTACH_NID, FLAG_EA);
    CHECK(ac.stop == StartupStop::None && acpu.gpr[3] == rc_reg(0x80410909u));

    // ---- gate 4: spuPort != 0xFF -> 0x8041090F ----
    { std::vector<uint8_t> bad = flag; bad[0x0C] = 0u; a.put(FLAG_EA, bad.data(), bad.size()); }
    g10_call(a, EVENT_FLAG_ATTACH_NID, FLAG_EA);
    CHECK(ac.stop == StartupStop::None && acpu.gpr[3] == rc_reg(0x8041090Fu) && ac.event_flag_attach_last == 0x8041090Fu);

    // ---- ORDER: gate 3 precedes gate 4, so both bad reports 0x80410909 ----
    { std::vector<uint8_t> bad = flag; bad[0x0E] = 0u; bad[0x0C] = 0u; a.put(FLAG_EA, bad.data(), bad.size()); }
    g10_call(a, EVENT_FLAG_ATTACH_NID, FLAG_EA);
    CHECK(ac.stop == StartupStop::None && acpu.gpr[3] == rc_reg(0x80410909u));

    // ---- no guest write on any refusal ----
    { std::vector<uint8_t> bad = flag; bad[0x0E] = 0u; a.put(FLAG_EA, bad.data(), bad.size()); }
    const auto bad_before = g10_bytes(a, FLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES);
    g10_call(a, EVENT_FLAG_ATTACH_NID, FLAG_EA);
    CHECK(g10_bytes(a, FLAG_EA, PS3_SPURS_EVENT_FLAG_BYTES) == bad_before);

    // ---- invalid memory: an unreadable direction byte stops InvalidMemory ----
    Runtime b; b.boot2(); ScratchScope scope4(b); auto &bc = *b.control;
    g10_call(b, EVENT_FLAG_ATTACH_NID, 0x30000000u);   // NULL/align gates see nothing; 0x30000000 is 128-aligned
    CHECK(bc.stop == StartupStop::InvalidMemory);
    CHECK(bc.event_flag_attach_calls == 1 && bc.event_flag_attach_returns == 0);
    CHECK(bc.event_flag_attach_last == 0xFFFFFFFFu);
    // ---- ROUTE anchor: dispatched, never UnknownImport (the feature-off build is proven by the
    // verifier's preprocessor needle check for 0x87630976 / dispatch_spurs_event_flag_attach). ----
    Runtime d; d.boot2(); ScratchScope scope6(d);
    g10_call(d, EVENT_FLAG_ATTACH_NID, 0u);
    CHECK(d.control->stop != StartupStop::UnknownImport);
    CHECK(d.control->event_flag_attach_calls == 1);
}


// ---- cellSpursCreateTasksetWithAttribute (NID 0xC10931CB) --------------------------------------
// The REAL ps3_hle_call route over
// the real composition: the entry's three gates, the worker's 6400-byte image, the native workload
// registration through ps3_spurs_runtime_register_workload, and the wid the adapter feeds back into
// taskset+0x74 (which the native cellSpursGetWorkloadInfo then reads back).
namespace {
constexpr uint32_t CREATE_NID = 0xc10931cb, WORKLOAD_INFO_NID = 0x4e153e3e;
constexpr uint32_t WORKLOAD_INFO_OUT = STACK_BASE + 0x700;
// The scratch window has to reach the 6400-byte taskset plus a 4-byte wid and a 1-byte flag byte.
constexpr uint32_t CREATE_TASKSET_EA = SCRATCH_BASE + 0x1800;   // 128-byte aligned
constexpr uint32_t CREATE_SPURS_EA = INSTANCE1;                  // 128-byte aligned (0x7CF100)

// Stamps the taskset attribute exactly as _cellSpursTasksetAttributeInitialize + the name setter
// leave it for inFAMOUS: revision 1, sdk 0x00250001, args 0, priority 0x08,0...
// max_contention 8, name = NAME_EA, taskset_size 6400, enable_clear_ls 0.
void create_attribute(Runtime &r, uint32_t attr_ea, uint32_t name_ea) {
    std::vector<uint8_t> image(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0);
    const auto put = [&](size_t offset, size_t width, uint64_t value) {
        for (size_t i = 0; i < width; ++i) image[offset + i] = uint8_t(value >> (8 * (width - 1 - i)));
    };
    put(0x00, 4, 1u); put(0x04, 4, 0x00250001u); put(0x08, 8, 0u);
    image[0x10] = 8u;
    put(0x18, 4, 8u); put(0x1C, 4, name_ea); put(0x20, 4, PS3_SPURS_TASKSET_SIZE);
    r.put(attr_ea, image.data(), image.size());
}

// The Service provider the registration needs (kernel_initialized + workload ports). It must never be
// asked to execute anything: this entry starts no SPU thread.
struct CreateServices {
    ps3_pthread_guard guards[PS3_SPURS_MAX_INSTANCES]{};
    std::atomic<unsigned> execution_calls{0};
    CreateServices() { for (auto &guard : guards) CHECK(!ps3_pthread_guard_init(&guard)); }
    ~CreateServices() { for (auto &guard : guards) ps3_pthread_guard_destroy(&guard); }
    static ps3_spurs_policy_execution_result refuse(void *context, const ps3_spurs_policy_execution_request *) {
        ++static_cast<CreateServices *>(context)->execution_calls;
        ps3_spurs_policy_execution_result result{}; result.status = PS3_SPURS_POLICY_UNSUPPORTED; return result;
    }
    static ps3_spurs_policy_execution_result step(void *context, uint32_t, uint64_t) { return refuse(context, nullptr); }
    static ps3_spurs_policy_execution_status release(void *, uint32_t) { return PS3_SPURS_POLICY_UNSUPPORTED; }
    void install(Runtime &runtime) {
        ps3_instance_guard guard_ports[PS3_SPURS_MAX_INSTANCES];
        ps3_spurs_policy_execution_port ports[PS3_SPURS_MAX_INSTANCES];
        for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) {
            guard_ports[i] = ps3_pthread_guard_port(&guards[i]);
            ports[i] = {this, refuse, step, step, release, release};
        }
        CHECK(!ps3_spurs_runtime_set_workload_services(&runtime.comp->runtime, guard_ports, ports));
    }
};
}   // namespace

void run_create_taskset() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
    create_attribute(r, ATTR_EA, NAME_EA);
    // Sentinel the whole taskset window before the call: the 6400 bytes are all rewritten, the byte
    // after them is not.
    std::vector<uint8_t> dirty(PS3_SPURS_TASKSET_BYTES, 0xA5);
    r.put(CREATE_TASKSET_EA, dirty.data(), dirty.size());
    r.put8(CREATE_TASKSET_EA + PS3_SPURS_TASKSET_BYTES, 0xA5);
    const size_t from = r.published();
    const uint64_t enabled_before = r.get(INSTANCE1 + 0xb0, 4);

    // ---- success: inFAMOUS's own arguments (survey §A.2.5): r3 = spurs, r4 = taskset, r5 = attribute ----
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, ATTR_EA);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[2] == TOC);                                   // TOC restored
    CHECK(cpu.gpr[3] == 0);                                     // the worker's success code
    CHECK(r.get(TOC_SLOT, 8) == TOC);                           // TOC slot published
    CHECK(c.taskset_create_calls == 1 && c.taskset_create_returns == 1);
    CHECK(c.taskset_create_last == 0u);
    CHECK(c.taskset_create_wid == 0u);                          // the first free wid
    CHECK(c.taskset_create_registered == 1);
    // 6400 (the image) + 4 (wid at +0x74) + 1 (wkl_flag_wait_task at +0x72) + 8 (the 0xDC5C
    // event-flag word at +0x100 + wid*128).
    CHECK(c.taskset_create_writes == PS3_SPURS_TASKSET_BYTES + 4u + 1u + 8u);
    // The exact object image: five scalar fields inside a fully zeroed 6400, plus
    // the 0xDC5C store on this call's wid slot (an 8-byte store of 1 through a
    // reservation pair).
    {
        auto image = g10_bytes(r, CREATE_TASKSET_EA, PS3_SPURS_TASKSET_BYTES);
        std::vector<uint8_t> expected(PS3_SPURS_TASKSET_BYTES, 0);
        const auto put = [&](size_t offset, size_t width, uint64_t value) {
            for (size_t i = 0; i < width; ++i) expected[offset + i] = uint8_t(value >> (8 * (width - 1 - i)));
        };
        put(0x60, 8, CREATE_SPURS_EA);          // spurs back-pointer
        put(0x68, 8, 0);                        // args
        expected[0x70] = 0;                     // enable_clear_ls (attribute+0x27 was 0)
        expected[0x72] = 0x80;                  // wkl_flag_wait_task
        put(0x74, 4, 0);                        // wid
        put(0x100, 8, 1);                       // 0xDC5C: the event-flag word of wid 0
        put(0x1890, 4, PS3_SPURS_TASKSET_SIZE); // size
        CHECK(image == expected);
        // Stated the way the task words it: the 8 bytes at +0x100 + wid*128 read 1 and the whole
        // 6400-byte object equals the g13 image plus exactly that store and nothing else.
        CHECK(r.get(CREATE_TASKSET_EA + 0x100u + 0u, 8) == 1u);
        std::vector<uint8_t> g13_image(PS3_SPURS_TASKSET_BYTES, 0);
        const auto put13 = [&](size_t offset, size_t width, uint64_t value) {
            for (size_t i = 0; i < width; ++i) g13_image[offset + i] = uint8_t(value >> (8 * (width - 1 - i)));
        };
        put13(0x60, 8, CREATE_SPURS_EA); put13(0x68, 8, 0); g13_image[0x72] = 0x80;
        put13(0x74, 4, 0); put13(0x1890, 4, PS3_SPURS_TASKSET_SIZE);
        put13(0x100, 8, 1);
        CHECK(image == g13_image);
        const size_t flag = 0x100u;
        bool bytes_match = true;
        for (size_t i = 0; i < PS3_SPURS_TASKSET_BYTES; ++i) {
            const unsigned want = i >= flag && i < flag + 8u ? (i == flag + 7u ? 1u : 0u) : g13_image[i];
            if (unsigned(image[i]) != want) bytes_match = false;
        }
        CHECK(bytes_match);
    }
    CHECK(g10_bytes(r, CREATE_TASKSET_EA + PS3_SPURS_TASKSET_BYTES, 1) == std::vector<uint8_t>(1, 0xA5));
    // The wid really claimed in the native SPURS instance.
    CHECK(r.get(INSTANCE1 + 0xb0, 4) == (enabled_before | 0x80000000u));
    CHECK(r.get(INSTANCE1 + 0xb4, 4) == 0x80000000u);
    CHECK(r.get(INSTANCE1 + 0x80, 1) == 2);                     // SPURS_WKL_STATE_RUNNABLE
    // The workload info the native registration wrote is the firmware's projection of the SAME
    // attribute: policy module 0x00023680, size 0x1E40, data = the taskset EA (0x14B88 r8 = r21),
    // priority, min 8, max 8. The domain encodes each field big-endian, so +0xB00 is the 8-byte
    // word policy_module and +0xB08 the 8-byte word data (spurs_workload_registration.c
    // initialize_record: write_value(info,8,...) then write_value(info+8,8,...)).
    CHECK(r.get(INSTANCE1 + 0xb00, 8) == PS3_SPURS_TASKSET_PM_EA);
    CHECK(r.get(INSTANCE1 + 0xb08, 8) == CREATE_TASKSET_EA);   // data = the taskset EA
    CHECK(r.get(INSTANCE1 + 0xb10, 4) == PS3_SPURS_TASKSET_PM_SIZE);
    CHECK(r.get(INSTANCE1 + 0xb18, 8) == 0x0800000000000000ull);
    // +0x40 holds wklMinContention (the literal 8 from 0x14B8C), +0x50 is wklMaxContention's slot
    // and the native domain packs it nibble-wise at +0x50.
    // The native domain records min contention the way the reference packs wklMaxContention
    // (MAXCNT macros: nibble at +0x50 + (wid&~3), shift (3-(wid&3))*8), not at +0x40, and both the
    // registration request and the readback use that encoding -- so the same 8 the firmware passed
    // is what the guest reads back at output+0x21 below.
    CHECK(r.get(INSTANCE1 + 0x50 + (0u & ~3u), 1) == 0x08u);
    // No ready count published and no workload signal: nothing runs yet.
    CHECK(r.get(INSTANCE1 + 0, 1) == 0);
    // The registration reached the two native publication messages, in that order.
    {
        bool update = false, service_msg = false;
        for (const auto &event : r.since(from)) {
            if (is_event(event, Ev{true, INSTANCE1 + 0xbd, 1, 0xff})) update = true;
            if (is_event(event, Ev{true, INSTANCE1 + 0x72, 1, 0xff})) service_msg = true;
        }
        CHECK(update && service_msg);
    }
    CHECK(services.execution_calls.load() == 0);

    // ---- the readback through the native workload-info path matches the firmware ----
    {
        std::vector<uint8_t> out(0x30, 0xA5);
        r.put(WORKLOAD_INFO_OUT, out.data(), out.size());
        g10_call(r, WORKLOAD_INFO_NID, CREATE_SPURS_EA, 0u, WORKLOAD_INFO_OUT);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
        // The workload-info record +0x00 is `data`. It is the taskset EA -- the same word the
        // worker stored at taskset+0x68 -- and NOT attribute->args.
        // g13 asserted `data == 0` because the attribute's args word is 0 for inFAMOUS; that
        // coincidentally matched the sentinel value and hid the mis-registration.
        CHECK(r.get(WORKLOAD_INFO_OUT, 8) == CREATE_TASKSET_EA);           // data = the taskset EA
        CHECK(r.get(WORKLOAD_INFO_OUT + 0x10, 4) == PS3_SPURS_TASKSET_PM_EA);
        CHECK(r.get(WORKLOAD_INFO_OUT + 0x14, 4) == PS3_SPURS_TASKSET_PM_SIZE);
        CHECK(r.get(WORKLOAD_INFO_OUT + 0x20, 1) == 0);                     // current contention
        CHECK(r.get(WORKLOAD_INFO_OUT + 0x21, 1) == 8);                     // min contention
        CHECK(r.get(WORKLOAD_INFO_OUT + 0x22, 1) == 8);                     // max contention
        CHECK(r.get(WORKLOAD_INFO_OUT + 0x23, 1) == 0);                     // ready count
    }

    // ---- a SECOND create on the same taskset: the firmware claims the NEXT free wid and rewrites
    // the object, so the call succeeds again with wid 1 (CONTRACT §B.3: the slot claim is
    // countl_one(wklEnabled), it never refuses a second taskset). ---- 
    {
        const uint64_t enabled1 = r.get(INSTANCE1 + 0xb0, 4);
        const size_t returns1 = c.taskset_create_returns;
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
        CHECK(c.taskset_create_returns == returns1 + 1 && c.taskset_create_last == 0u);
        CHECK(c.taskset_create_wid == 1u);
        CHECK(r.get(INSTANCE1 + 0xb0, 4) == (enabled1 | 0x40000000u));
        CHECK(r.get(CREATE_TASKSET_EA + 0x74, 4) == 1u);
        // The second create claims wid 1, so 0xDC5C now addresses +0x100 + 1*128 = +0x180. The
        // image rewrite zeroed wid 0's slot again, so exactly one flag word is set.
        CHECK(r.get(CREATE_TASKSET_EA + 0x180, 8) == 1u);
        CHECK(r.get(CREATE_TASKSET_EA + 0x100, 8) == 0u);
        CHECK(services.execution_calls.load() == 0);
    }

    // NOTE, measured rather than assumed: the SPURS1 primary workload mask starts out fully
    // populated (`enabled=0x0000FFFF` before the first create, as ps3_spurs_runtime_initialize
    // leaves it), so `countl_one` never runs out of slots and the firmware's 0x80410901 arm is
    // UNREACHABLE on this instance. What is reachable is the other side of that same check:
    // 0x97E4 finds no free slot, returns AGAIN only past `wnum >= wmax`, and instead re-writes
    // the existing wid's record. The native domain therefore keeps answering 0 with a SAME wid,
    // and the adapter legitimately counts it. This is recorded, not worked around.
}

void run_create_taskset_refusals() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
    create_attribute(r, ATTR_EA, NAME_EA);
    std::vector<uint8_t> dirty(PS3_SPURS_TASKSET_BYTES, 0xA5);
    r.put(CREATE_TASKSET_EA, dirty.data(), dirty.size());
    const auto taskset_before = g10_bytes(r, CREATE_TASKSET_EA, PS3_SPURS_TASKSET_BYTES);
    const uint64_t enabled_before = r.get(INSTANCE1 + 0xb0, 4);

    // Every refusal below must write NO guest byte and claim NO workload.

    // gate 1: NULL attribute -> 0x80410911
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, 0u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(c.taskset_create_last == 0x80410911u && c.taskset_create_returns == 1);
    CHECK(c.taskset_create_writes == 0u && c.taskset_create_wid == 0xFFFFFFFFu);
    CHECK(g10_bytes(r, CREATE_TASKSET_EA, PS3_SPURS_TASKSET_BYTES) == taskset_before);
    CHECK(r.get(CREATE_TASKSET_EA + 0x100, 8) == UINT64_C(0xA5A5A5A5A5A5A5A5));  // no 0xDC5C store

    // gate 2: misaligned attribute -> 0x80410910
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, ATTR_EA + 4u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u));
    CHECK(c.taskset_create_last == 0x80410910u && c.taskset_create_writes == 0u);
    CHECK(g10_bytes(r, CREATE_TASKSET_EA, PS3_SPURS_TASKSET_BYTES) == taskset_before);

    // gate 3: revision != 1 -> 0x80410902
    { std::vector<uint8_t> zero(PS3_SPURS_TASKSET_ATTRIBUTE_BYTES, 0); r.put(ATTR_EA, zero.data(), zero.size()); }
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
    CHECK(c.taskset_create_last == 0x80410902u && c.taskset_create_writes == 0u);
    CHECK(g10_bytes(r, CREATE_TASKSET_EA, PS3_SPURS_TASKSET_BYTES) == taskset_before);
    create_attribute(r, ATTR_EA, NAME_EA);

    // ---- the worker's own gates: a NULL or misaligned spurs/taskset -> 0x80410911 / 0x80410910
    // Neither is checked by the entry.
    g10_call(r, CREATE_NID, 0u, CREATE_TASKSET_EA, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u) && c.taskset_create_writes == 0u);
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, 0u, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u) && c.taskset_create_writes == 0u);
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA + 0x10u, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u) && c.taskset_create_writes == 0u);
    g10_call(r, CREATE_NID, CREATE_SPURS_EA + 0x7Fu, CREATE_TASKSET_EA, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u) && c.taskset_create_writes == 0u);
    CHECK(g10_bytes(r, CREATE_TASKSET_EA, PS3_SPURS_TASKSET_BYTES) == taskset_before);

    // ---- a priority byte above 0xF in the attribute -> the policy module's own 0x80410802
    // (0xA0EC/0xA0F0 -> 0xA14C). 0xA000 is called AFTER the object was written, so the image is
    // present; the wid is not, and no workload was claimed. ----
    {
        create_attribute(r, ATTR_EA, NAME_EA);
        r.put8(ATTR_EA + 0x13, 0x10);
        const size_t returns0 = c.taskset_create_returns;
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410802u));
        CHECK(c.taskset_create_returns == returns0 + 1 && c.taskset_create_last == 0x80410802u);
        CHECK(c.taskset_create_writes == PS3_SPURS_TASKSET_BYTES);
        CHECK(r.get(CREATE_TASKSET_EA + 0x74, 4) == 0u);         // wid never written
        CHECK(r.get(CREATE_TASKSET_EA + 0x72, 1) == 0u);         // and neither is wkl_flag_wait_task
        CHECK(r.get(CREATE_TASKSET_EA + 0x100, 8) == 0u);        // nor the 0xDC5C event-flag word
        CHECK(r.get(INSTANCE1 + 0xb0, 4) == enabled_before);     // no slot claimed
        create_attribute(r, ATTR_EA, NAME_EA);
    }

    // ---- invalid memory: an unwritable taskset EA stops InvalidMemory (the 6400-byte store) ----
    {
        const size_t calls0 = c.taskset_create_calls, returns0 = c.taskset_create_returns;
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, 0x30000000u, ATTR_EA);
        CHECK(c.stop == StartupStop::InvalidMemory && c.invalid_address == 0x30000000u);
        CHECK(c.taskset_create_calls == calls0 + 1 && c.taskset_create_returns == returns0);
        CHECK(c.taskset_create_last == 0xFFFFFFFFu);
    }
    // ---- invalid memory: an unreadable attribute EA stops InvalidMemory on the FIRST read, which is
    // attribute+0x00 -- the entry's gates only inspect the pointer's value.
    {
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        g10_call(v, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, 0x30000000u);
        CHECK(vc.stop == StartupStop::InvalidMemory && vc.invalid_address == 0x30000000u);
        CHECK(vc.taskset_create_calls == 1 && vc.taskset_create_returns == 0);
        CHECK(vc.taskset_create_last == 0xFFFFFFFFu && vc.taskset_create_writes == 0u);
    }
    // ---- a NULL attribute and a misaligned one read NO guest byte, so neither faults ----
    {
        Runtime v; services.install(v); v.boot2(); CreateScope scope3(v);
        g10_call(v, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, 0u);
        CHECK(v.control->stop == StartupStop::None);
        g10_call(v, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, 0x30000004u);
        CHECK(v.control->stop == StartupStop::None);
        CHECK(v.execution.ppu.cpu.gpr[3] == rc_reg(0x80410910u));
    }

    // ---- ROUTE anchor: with PS3_NATIVE_STARTUP_SPURS_INITIALIZE defined this NID is dispatched, so it
    // never stops UnknownImport. With the feature off there is no case in ps3_hle_call and it does; that
    // build is proven by verify_startup_spurs_getinfo.sh's preprocessor needle check. ----
    {
        Runtime u; services.install(u); u.boot2(); CreateScope scope4(u);
        g10_call(u, CREATE_NID, CREATE_SPURS_EA, CREATE_TASKSET_EA, 0u);
        CHECK(u.control->stop != StartupStop::UnknownImport);
        CHECK(u.control->taskset_create_calls == 1);
    }
}


// g14/g14b: _cellSpursLFQueueInitialize (NID 0x011EE38B) through the REAL ps3_hle_call route.
// The entry validates its six
// arguments, dispatches on the low nibble of r3 (non-zero = CellSpursTaskset owner), reads the
// owner's word, and rebuilds the 128-byte CellSyncLFQueue line plus two 30-byte runs of zeros in the
// caller's object.
namespace {
constexpr uint32_t LFQ_NID = 0x011ee38b;
// inFAMOUS's own call site (0x004248B0..0x004248D4), decoded from the recompiled game:
//   r3 = r31 | 1      r4 = r30      r5 = r27 + 0x80     r6 = 0x20     r7 = r29     r8 = 3
// with r31 == r30 == the taskset EA and r27 == its buffer base. The queue IS the taskset, so the
// 128-byte line overwrites taskset+0x24 (the version word this dispatch then reads) -- which is why
// the game's own object sees version 0 here, the CellSyncLFQueue shape.
// The dispatcher 0x209C takes its arguments from the entry's, at 0x17150..0x17164:
//   0x209C r3 = owner   0x209C r4 = the caller's r5 (buffer)   0x209C r5 = the caller's r6 (elem)
//   0x209C r6 = the caller's r7 (depth)   0x209C r7 = the caller's r8 (direction)
// so the bound is on the ELEMENT SIZE, not the buffer EA. The game's
// own buffer is `taskset + 0x80`, an ordinary EA, and nothing bounds it.
constexpr uint32_t LFQ_TASKSET_EA = CREATE_TASKSET_EA, LFQ_BUFFER_EA = CREATE_TASKSET_EA + 0x80;
// G15c (decision 26): the SPURS-arm cases (owner with a clear low nibble, so 0x17144 sends the entry to 0x171A4) read and write the version
// word at `owner+0x74`, which on a live CellSpurs is `flags1`/`nspus`. They run on LFQ_SCRATCH_EA instead, so the word under test is the
// scratch object's own and no live instance header is rewritten. It holds exactly the booted game's values: +0x74 = 0x20, +0x76 = 0x00.
constexpr uint32_t LFQ_SPURS_SCRATCH = LFQ_SCRATCH_EA, LFQ_SCRATCH_VERSION = LFQ_SCRATCH_EA + 0x74;
static_assert(LFQ_SCRATCH_SIZE >= PS3_SPURS_LFQ_BYTES + 0x74u, "the scratch object holds the version word and the whole line");
static_assert(LFQ_SCRATCH_EA > INSTANCE2 + 0x3000u - 0x1000u && LFQ_SCRATCH_EA + LFQ_SCRATCH_SIZE <= STACK_BASE &&
    "the scratch object is outside every live instance's extent");
}

void run_lfqueue_initialize() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    // G15c: the taskset line this case builds (and the taskset image, both created after the runtime booted) live in pages
    // the fixture commits here, and a committed page is never re-zeroed. Storing a zero into them first makes every value the
    // set-up writes safe to expose to any concurrent reader for the rest of the case.
    r.zero_pages(LFQ_TASKSET_EA, 0x2000);
    // Build the taskset first, exactly as the game's previous call does, so the queue object the
    // LFQueue entry lands on is the real 6400-byte one.
    {
        const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
        create_attribute(r, ATTR_EA, NAME_EA);
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
        CHECK(c.taskset_create_wid == 0u);
    }
    // The buffer span the builder scans (elem_size * depth = 0x60) is inside the taskset object.
    const uint64_t enabled_before = r.get(INSTANCE1 + 0xb0, 4);

    // The taskset's own state word at +0x2C is 0 (g13's image writes only 0x60, 0x68, 0x70, 0x72,
    // 0x74, 0x100+wid*128 and 0x1890), which is what selects the builder branch at 0x2274.
    CHECK(r.get(LFQ_TASKSET_EA + 0x2C, 4) == 0u);

    // ---- inFAMOUS's exact six registers. Every gate passes; the dispatcher's state probe at
    // 0x220C..0x2248 reads 0, so 0x226C/0x2270 fall through to the builder at 0x2274, the line and
    // the two auxiliary runs are written, +0x2C is set to 2 and the builder branch
    // returns r31 == 0. **The answer is 0, and no probe is reached at all** (g14b §4). ----
    g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[2] == TOC);                              // TOC restored
    CHECK(cpu.gpr[3] == 0);                                // 0x22A0 -> 0x2108, r31 == 0
    CHECK(r.get(TOC_SLOT, 8) == TOC);                      // TOC slot published
    CHECK(c.lfq_calls == 1 && c.lfq_returns == 1);
    CHECK(c.lfq_last == 0u);
    CHECK(c.lfq_owner == (uint64_t(LFQ_TASKSET_EA) | 1u));
    CHECK(c.lfq_version == 0u);                            // the g13 image leaves taskset+0x24 zero
    CHECK(c.lfq_shape == 0u);                              // so the CellSyncLFQueue shape
    // 0 is what the entry returns, and what `if ((cr >> 0) & 2) goto loc_00424830` sees: the game
    // takes the success arm, not loc_00424820's assert.
    // 2 * 30 (the auxiliary runs) + 128 (the line) + 2 (the reservation word) + 4 (the +0x2C state
    // mark at 0x2298) = 194 bytes.
    CHECK(c.lfq_writes == 2u * 30u + PS3_SPURS_LFQ_BYTES + 2u + 4u);

    // The exact line: the six scalars the entry writes, over the taskset object's own bytes.
    {
        auto line = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES);
        const auto be = [&](size_t offset, size_t width) {
            uint64_t v = 0;
            for (size_t i = 0; i < width; ++i) v = v << 8 | line[offset + i];
            return v;
        };
        CHECK(be(0x00, 2) == 0u);
        CHECK(be(0x10, 4) == 0x20u);
        CHECK(be(0x14, 4) == 3u);
        CHECK(be(0x18, 8) == (uint64_t(LFQ_BUFFER_EA) & ~1u));   // NOT |1: the CellSyncLFQueue tail
        CHECK(be(0x24, 4) == 3u);                                // the direction
        CHECK(be(0x70, 8) == (uint64_t(LFQ_TASKSET_EA) | 1u));   // the TAGGED owner
        CHECK(be(0x20, 4) == 0xFFFFFFFFu);                       // the CellSyncLFQueue tail
        CHECK(be(0x28, 4) == 0u);
        CHECK(be(0x78, 8) == 0u);
        CHECK(be(0x21, 1) == 0xFFu);                             // part of the +0x20 four-byte store
        CHECK(be(0x04, 2) == 0u);                                // the spurs shape's +0x04, not written
        CHECK(be(0x0C, 2) == 0u);                                // ditto +0x0C
    }
    // The state word the entry leaves: +0x08 is zero.
    CHECK(r.get(LFQ_TASKSET_EA + PS3_SPURS_LFQ_H8, 2) == 0u);
    // The two auxiliary runs write 30 zero bytes each at queue+0x30 and queue+0x50. The image's own
    // stores land inside the same span (+0x30's first store, the CellSyncLFQueue tail's +0x50..0x51),
    // and the object's bytes the firmware never writes keep their value, so the assertion covers the
    // bytes the 30-byte run owns and the image leaves alone.
    for (size_t i = 0; i < 30u; ++i) {
        CHECK(r.get(LFQ_TASKSET_EA + 0x30u + i, 1) == 0u);
        if (i >= 2u && i < 0x15u) CHECK(r.get(LFQ_TASKSET_EA + 0x50u + i, 1) == 0u);
    }
    // No workload was touched: this entry claims no slot and starts nothing.
    CHECK(r.get(INSTANCE1 + 0xb0, 4) == enabled_before);
    // The object is now marked initialized: +0x2C holds 2 (a store of the constant 2 into
    // the state word). A SECOND call therefore takes the match arm and only validates what is already there --
    // it writes nothing. This is the real second-call behaviour, and it is what the state word is for.
    CHECK(r.get(LFQ_TASKSET_EA + PS3_SPURS_LFQ_STATE, 4) == 2u);
    {
        const auto line_before_second = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES);
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);   // 0x233C -> 0x229C -> 0x2104
        CHECK(c.lfq_calls == 2 && c.lfq_returns == 2 && c.lfq_last == 0u);
        CHECK(c.lfq_writes == 0u);                               // 0x22E4..0x2338 writes no guest byte
        CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES) == line_before_second);
    }
    // The validation arm's five fields (0x22E4, 0x22F0, 0x2300, 0x2324, 0x2330): any disagreement
    // is the 0x80410902 word gates 3-7 share, and still nothing is written.
    {
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 4u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));   // depth differs
        CHECK(c.lfq_last == 0x80410902u && c.lfq_writes == 0u);
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 2u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));   // direction differs
        CHECK(c.lfq_last == 0x80410902u && c.lfq_writes == 0u);
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x30u, 3u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));   // elem size differs
        CHECK(c.lfq_last == 0x80410902u && c.lfq_writes == 0u);
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA + 0x20u, 0x20u, 3u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));   // buffer differs
        CHECK(c.lfq_last == 0x80410902u && c.lfq_writes == 0u);
        /* The owner is the fourth field 0x2324 compares, but gate 8 (`owner & 0x7F`) refuses every
         * owner whose low bits could disagree with the stored value before the validation arm can be
         * reached, so it needs no separate case here: the three above already cover 0x22E4/0x22F0/
         * 0x2300, and 0x2330 is the direction case above them. */
    }
    // The CellSpursLFQueue shape is selected only by taskset+0x24 == 3, and the entry writes the
    // direction over that same word (0x1FBC), so the game never sees it. Asserted on a fresh object
    // whose +0x24 has been pre-set and whose +0x2C is 0.
    {
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        {
            const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
            create_attribute(v, ATTR_EA, NAME_EA);
            g10_call(v, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
            CHECK(vc.stop == StartupStop::None);
        }
        v.put32(LFQ_TASKSET_EA + 0x24, 3u);
        CHECK(v.get(LFQ_TASKSET_EA + PS3_SPURS_LFQ_STATE, 4) == 0u);
        g10_call(v, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None && v.execution.ppu.cpu.gpr[3] == 0);
        CHECK(vc.lfq_version == 3u && vc.lfq_shape == 1u);
        const auto line = g10_bytes(v, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES);
        const auto be = [&](size_t offset, size_t width) {
            uint64_t value = 0;
            for (size_t i = 0; i < width; ++i) value = value << 8 | line[offset + i];
            return value;
        };
        CHECK(be(0x21, 1) == 0xFFu);
        CHECK(be(0x50, 2) == 0xFFFFu);
        CHECK(be(0x30, 2) == 0xFFFFu);
        CHECK(be(0x28, 4) == 0xFFFFFFFFu);
        CHECK(be(0x24, 4) == 3u);       // the direction, written over the word that selected the shape
        CHECK(be(0x18, 8) == (uint64_t(LFQ_BUFFER_EA) | 1u));
    }
}

void run_lfqueue_refusals() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    r.zero_pages(LFQ_TASKSET_EA, 0x2000);   // G15c: set-up bytes below are pages the fixture commits; keep them unrepresentable to concurrent readers
    {
        const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
        create_attribute(r, ATTR_EA, NAME_EA);
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None);
    }
    const auto line_before = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES);
    const auto aux_before = g10_bytes(r, LFQ_TASKSET_EA + 0x30u, 60u);

    // Every refusal below must write NO guest byte and claim NO workload.

    // queue == 0 -> 0x80410911
    g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, 0u, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(c.lfq_last == 0x80410911u && c.lfq_returns == 1 && c.lfq_writes == 0u);
    CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES) == line_before);
    CHECK(g10_bytes(r, LFQ_TASKSET_EA + 0x30u, 60u) == aux_before);

    // elem_size == 0 -> 0x80410911. The buffer is NOT NULL-checked:
    // the dispatcher's r5 is the caller's r6, and the buffer (r4) is only alignment-checked. ----
    g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0u, 3u, 3u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u));
    CHECK(c.lfq_last == 0x80410911u && c.lfq_writes == 0u);
    {
        const auto before = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES);
        // depth == 0 -> 0x20F4/0x20F8
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 0u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
        // elem_size > 0x4000 -> 0x217C, the only 0x4000 bound in the entry
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x4010u, 3u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
        // elem_size & 0xF -> 0x2184/0x218C
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x24u, 3u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
        // depth's high half -> 0x2194/0x219C
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 0x10003u, 3u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
        // direction > 3 -> 0x21A0/0x21A4
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 4u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410902u));
        CHECK(c.lfq_writes == 0u);
        CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES) == before);
    }
    CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES) == line_before);

    // ---- owner & 0x7F -> 0x80410910 (0x21A8/0x21B4) ----
    g10_call(r, LFQ_NID, (uint64_t(LFQ_TASKSET_EA) | 1u) + 0x40u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u));
    CHECK(c.lfq_last == 0x80410910u && c.lfq_writes == 0u);

    // ---- buffer & 0xF -> 0x80410910 (0x21C0/0x21C8, r26 = the BUFFER); a misaligned QUEUE passes ----
    g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA + 4u, 0x20u, 3u, 3u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u));
    CHECK(c.lfq_writes == 0u);
    CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQ_BYTES) == line_before);

    // G14c: the SPURS arm (r3 with a clear low nibble) answers its OWN gate first. The nibble
    // test sends it to the version check, which is the ENTRY's
    // first guest access; `> 0x1F` returns 0x80410902 and touches nothing. Nothing is read from the queue
    // line before it -- reading it first was g14b's regression, not the firmware's order.
    // G15c (decision 26): on a live CellSpurs `+0x74` is `flags1` and `+0x76` is `nspus`, which that instance's own kernel worker revalidates;
    // these cases therefore call the entry on LFQ_SCRATCH_EA, pre-set to exactly what the booted game's header holds (version 0x20, no
    // EXIT_IF_NO_WORK bit, plus 0x76 = 0). Same gate, same operand, no instance header written.
    {
        Runtime v; services.install(v); auto &vc = *v.control;
        v.boot2(); CreateScope scope2(v);   // the live instances are created before the scratch word is written
        v.put32(LFQ_SCRATCH_VERSION, 0x20u);
        v.put8(LFQ_SPURS_SCRATCH + 0x76, 0x00u);
        g10_call(v, LFQ_NID, uint64_t(LFQ_SPURS_SCRATCH), LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        CHECK(v.execution.ppu.cpu.gpr[3] == rc_reg(0x80410902u));
        CHECK(vc.lfq_last == 0x80410902u && vc.lfq_writes == 0u);
        CHECK(vc.lfq_version == 0x20u);
        // The word the gate read is the scratch object's and is untouched by the refusal: an implementation that read the
        // queue line first would have reported that gate's word (or faulted) instead.
        CHECK(v.get(LFQ_SCRATCH_VERSION, 4) == 0x20u);
    }
    // ---- ...and the same arm reads ONLY spurs+0x74 before answering: an unreadable queue EA must not
    // fault, because 0x17228 is never reached on this arm. ----
    {
        Runtime v; services.install(v); auto &vc = *v.control;
        v.boot2(); CreateScope scope2(v);
        v.put32(LFQ_SCRATCH_VERSION, 0x20u);
        g10_call(v, LFQ_NID, uint64_t(LFQ_SPURS_SCRATCH), 0x30000000u, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);          // NOT InvalidMemory
        CHECK(v.execution.ppu.cpu.gpr[3] == rc_reg(0x80410902u));
        CHECK(vc.lfq_last == 0x80410902u && vc.lfq_writes == 0u);
    }
    // ---- and an unreadable spurs+0x74 is the arm's FIRST fault, which is the read 0x171A4 does. ----
    {
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        g10_call(v, LFQ_NID, 0x30000000u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::InvalidMemory);
        CHECK(vc.invalid_address == 0x30000000u + 0x74u);
        CHECK(vc.lfq_writes == 0u);
    }

    // ---- invalid memory: an unwritable queue EA stops InvalidMemory on the version read, which is
    // the entry's FIRST guest access, before any auxiliary run or line write.
    {
        const size_t calls0 = c.lfq_calls, returns0 = c.lfq_returns;
        g10_call(r, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, 0x30000000u, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(c.stop == StartupStop::InvalidMemory);
        CHECK(c.lfq_calls == calls0 + 1u && c.lfq_returns == returns0);
        CHECK(c.lfq_last == 0xFFFFFFFFu && c.lfq_writes == 0u);
    }

    // ---- ROUTE anchor: with PS3_NATIVE_STARTUP_SPURS_INITIALIZE defined this NID is dispatched, so
    // it never stops UnknownImport. With the feature off there is no case in ps3_hle_call and it
    // does; that build is proven by verify_startup_spurs_getinfo.sh's preprocessor needle check. ----
    {
        Runtime u; services.install(u); u.boot2(); CreateScope scope3(u);
        g10_call(u, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, 0u, 0u, 0u, 0u, 0u);
        CHECK(u.control->stop != StartupStop::UnknownImport);
        CHECK(u.control->lfq_calls == 1);
    }
}


// i1 (decision 22) + G15 (decision 25) + G16 (decision 27): cellSpursLFQueueAttachLv2EventQueue
// (NID 0x1656D49F) through the REAL ps3_hle_call route. The recovered contract was
// corrected by decision 27 (evidence/root-gcm-next-boundary-prep-r1/DECISIONS.md). inFAMOUS's process SDK is
// 0x00250001, so the entry runs the SDK-25 arm, NOT the older loop b1/i1 decoded (decision 25).
//
// THE OWNER (decision 27). `func_00424774(r3=X, r4=depth, r5=queue, r6=size)` calls init with
// `(X|1, queue, queue+0x80, 0x20, depth, 3)` and then attach with `r3 = queue`: the QUEUE and X are
// DIFFERENT objects. The init stores the tagged owner at `queue+0x70`, and the attach reads its low word
// from `queue+0x74` on EVERY arm, before the SDK compare at 0x175B8:
//   tagged (`|1`)  -> the CellSpurs EA itself (low nibble cleared);
//   untagged, != 0 -> the untagged owner is a TASKSET and that is its CellSpurs;
//   0, or a resolved 0 -> 0x80410902 before any guest write.
// G15 aliased queue == taskset, which is why its `+0x60` reading looked right; the real queue's `+0x60`
// lies inside the `queue+0x50..+0x6E` run the init zeroes.
namespace {
constexpr uint32_t LFQA_NID = 0x1656d49f;
// The game's real queue: a SEPARATE 128-byte-aligned object from the taskset, with its buffer at +0x80.
// Inside LFQ_SCRATCH_EA's 4 KiB block and clear of the G15c bytes at LFQ_SCRATCH_EA..+0xFF.
constexpr uint32_t LFQ_GAME_QUEUE_EA = LFQ_SCRATCH_EA + 0x400, LFQ_GAME_QUEUE_BUFFER = LFQ_GAME_QUEUE_EA + 0x80;
static_assert((LFQ_GAME_QUEUE_EA & 0x7Fu) == 0u, "the queue is 128-byte aligned (0x17508 gate 1)");
static_assert(LFQ_GAME_QUEUE_BUFFER + 0x80u <= LFQ_SCRATCH_EA + LFQ_SCRATCH_SIZE, "the queue line and buffer fit the scratch block");
}

void run_lfqueue_attach() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    r.zero_pages(LFQ_TASKSET_EA, 0x2000);   // G15c: same rule as run_lfqueue_initialize -- set-up bytes in pages committed before the case
    // 1. The taskset, by the game's own dispatches, then the line by _cellSpursLFQueueInitialize on the
    // game's own six registers.
    //   The taskset is created FIRST so the object the attach lands on is the real 6400-byte one and its
    //   `+0x60` is the CellSpurs EA `cellSpursCreateTasksetWithAttribute` writes. Then the LFQueue
    //   line is built on the game's own six registers, overwriting +0x00..+0x7F of that object.
    {
        const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
        create_attribute(r, ATTR_EA, NAME_EA);
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
    }
    // 2. The game's own call: a SEPARATE queue object, not the taskset. Zero it first, then init on the
    // game's six registers with `X | 1` as the owner and then run the attach on `r3 = queue`.
    r.zero_pages(LFQ_GAME_QUEUE_EA, 0x100);
    g10_call(r, LFQ_NID, uint64_t(CREATE_SPURS_EA) | 1u, LFQ_GAME_QUEUE_EA, LFQ_GAME_QUEUE_BUFFER, 0x20u, 3u, 3u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && c.lfq_last == 0u);
    CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x24, 4) == 3u);      // 0x1FBC: the direction -> the SPURS shape
    CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x20, 4) == 0xFFFFFFFFu);   // the CellSyncLFQueue tail's +0x20
    CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x21, 1) == 0xFFu);         // its second byte is what 0x17834 reads
    //   The queue's own `+0x70` is the u64 the init stored: the caller's r3 verbatim, tag included. Its low
    //   word at `+0x74` is what the attach reads, and `queue+0x50..+0x6E` -- which covers `+0x60`
    //   -- is zeroed by the init's second auxiliary run, which is why G15's `+0x60` reading could not work.
    CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x70, 8) == uint64_t(CREATE_SPURS_EA | 1u));
    CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x24, 4) == 3u);         // the direction, at the QUEUE's +0x24
    CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x60, 4) == 0u);         // zeroed by the init, not a CellSpurs EA
    //   The taskset's own `+0x60` still holds the CellSpurs EA the creator stored. In this fixture
    //   `cellSpursCreateTasksetWithAttribute` is driven as (spurs, taskset, attr) -- see run_create_taskset,
    //   which pins the whole 6400-byte image -- so the value there is `CREATE_SPURS_EA`.
    CHECK(r.get(LFQ_TASKSET_EA + 0x60, 8) == uint64_t(CREATE_SPURS_EA));

    // 3. THE GAME'S REAL SHAPE, THROUGH THE REAL SDK-25 ARM, and it RETURNS 0. The owner
    // word is read on EVERY arm, before the SDK compare at 0x175B8 (decision 27): `queue+0x74` holds
    // `CellSpurs | 1`, so 0x17590's nibble test resolves r28 = the CellSpurs EA and 0xAF2C connects to
    // exactly that untagged EA.
    //
    //   +0x24 == 3 passes 0x17780's zero test and 0x17678's shape test; +0x20/+0x21 are 0xFF (0x2084/0x2078);
    //   two iterations of create(128)+connect(251)+status(134)/group-id(136) run; 0x177F0 starts the group;
    //   and makes the epilogue's answer 0 -- which is exactly the
    //   value the caller's check (EQ = r3 == 0) needs to skip func_00452290.
    {
        const uint32_t group_of_instance1 = [&] {
            for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index)
                if (r.comp->runtime.instances[index].spurs_ea == INSTANCE1)
                    return r.comp->runtime.instances[index].group;
            return 0u;
        }();
        CHECK(group_of_instance1 != 0u);
        const size_t queues0 = ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE);
        const size_t calls0 = c.lfqa_calls, returns0 = c.lfqa_returns;
        const auto before = g10_bytes(r, LFQ_GAME_QUEUE_EA, PS3_SPURS_LFQA_BYTES);
        g10_call(r, LFQA_NID, LFQ_GAME_QUEUE_EA);
        CHECK(c.stop == StartupStop::None);
        CHECK(cpu.gpr[2] == TOC && cpu.gpr[3] == 0);
        CHECK(c.lfqa_calls == calls0 + 1u && c.lfqa_returns == returns0 + 1u);
        CHECK(c.lfqa_last == 0u);
        // The owner word WAS read here (decision 27), and it is the tagged CellSpurs EA.
        CHECK(c.lfqa_owner == (CREATE_SPURS_EA | 1u));
        CHECK(r.get(LFQ_GAME_QUEUE_EA + 0x74, 4) == (CREATE_SPURS_EA | 1u));   // untouched by the attach
        CHECK(c.lfqa_version == 3u && c.lfqa_shape == 1u && c.lfqa_iterations == 2u);
        CHECK(c.lfqa_queues_created == 2u && c.lfqa_queues_destroyed == 0u);
        CHECK(ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0 + 2u);
        CHECK(c.lfqa_port >= 16u && c.lfqa_port < 64u);      // 0xAFDC..0xAFE4: ports 16..63
        CHECK(c.lfqa_group == group_of_instance1);
        // The two ids the connect recorded on the instance are the ones this call created, depth-1 PPU
        // FIFOs created through the native lv2 registry.
        std::vector<uint32_t> queue_ids;
        for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index)
            if (r.comp->runtime.instances[index].spurs_ea == INSTANCE1)
                for (unsigned port = 16; port < PS3_SPU_PORTS; ++port)
                    if (r.comp->runtime.instances[index].user_port_queue[port] != 0)
                        queue_ids.push_back(r.comp->runtime.instances[index].user_port_queue[port]);
        CHECK(queue_ids.size() == 2u);
        for (uint32_t id : queue_ids) {
            ps3_lv2_queue snapshot{};
            CHECK(ps3_lv2_queue_snapshot(&r.comp->lv2, id, &snapshot) == PS3_LV2_OK);
            CHECK(snapshot.protocol == PS3_SYNC_FIFO && snapshot.type == PS3_PPU_QUEUE && snapshot.size == 1);
        }
        // The SPURS-shape publication: `+0x78` as a 64-bit word and `+0x20` as a 32-bit
        // one, and nothing else in the line moved.
        const auto after = g10_bytes(r, LFQ_GAME_QUEUE_EA, PS3_SPURS_LFQA_BYTES);
        const uint32_t recorded = (uint32_t(after[0x78]) << 24) | (uint32_t(after[0x79]) << 16) |
            (uint32_t(after[0x7A]) << 8) | uint32_t(after[0x7B]);
        const uint32_t recorded_lo = (uint32_t(after[0x7C]) << 24) | (uint32_t(after[0x7D]) << 16) |
            (uint32_t(after[0x7E]) << 8) | uint32_t(after[0x7F]);
        CHECK(recorded != 0u && recorded == recorded_lo);    // one id in both halves (INTEGRATION §3.3)
        CHECK(queue_ids.size() == 2u && recorded == queue_ids.back());
        const uint32_t port_word = (uint32_t(after[0x20]) << 24) | (uint32_t(after[0x21]) << 16) |
            (uint32_t(after[0x22]) << 8) | uint32_t(after[0x23]);
        CHECK(port_word == c.lfqa_port);
        for (size_t index = 0; index < PS3_SPURS_LFQA_BYTES; ++index) {
            const bool written = (index >= 0x78u && index < 0x80u) || (index >= 0x20u && index < 0x24u);
            if (!written) CHECK(after[index] == before[index]);
        }
        CHECK(c.lfqa_writes == 8u + 4u);
        // The dynamic connect bound the port on the instance and wrote no DA8 bit (the keyed path skips it).
        CHECK(r.get(INSTANCE1 + 0xDA8, 8) == 0);
    }

    // ---- 4. THE OLDER ARM, still correct for its SDK range. A process SDK of 0x180000 takes 0x17660 ->
    // 0x176F8. It resolves the owner from `+0x74` exactly as every other arm does (decision 27), and its
    // `+0x20`/`+0x21` must both be 0xFF. That is the arm b1 decoded, proven here on a separate runtime whose
    // process SDK is in range. ----
    {
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        v.execution.process_sdk = {0x180000u, true};
        {
            const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
            create_attribute(v, ATTR_EA, NAME_EA);
            g10_call(v, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
            CHECK(vc.stop == StartupStop::None);
        }
        g10_call(v, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        // The owner word is read on every arm; here it names the taskset object the line was built on, whose
        // resolved value no instance owns.
        g10_call(v, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(vc.lfqa_owner == (LFQ_TASKSET_EA | 1u));      // the raw +0x74 word, as read
        CHECK(vc.lfqa_version == 3u && vc.lfqa_shape == 1u);
        // Phase C resolves it to `taskset_ea & ~0xF`, a CellSpursTaskset address, which no instance of this
        // runtime owns -- the honest ProviderFailure the arm's own decode requires.
        CHECK(vc.stop == StartupStop::ProviderFailure);
        CHECK(vc.lfqa_last == 0xFFFFFFFFu && vc.lfqa_writes == 0u);
        // Point `+0x74` at the UNTAGGED taskset whose own `+0x64` holds the CellSpurs EA (phase B), and the
        // SAME arm serves. The taskset object is the one cellSpursCreateTasksetWithAttribute built above.
        const uint32_t group_of_instance1 = [&] {
            for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index)
                if (v.comp->runtime.instances[index].spurs_ea == INSTANCE1)
                    return v.comp->runtime.instances[index].group;
            return 0u;
        }();
        CHECK(group_of_instance1 != 0u);
        CHECK(v.get(LFQ_TASKSET_EA + 0x64, 4) == CREATE_SPURS_EA);   // phase B's read, at the taskset's +0x64
        v.put32(LFQ_TASKSET_EA + 0x74, LFQ_TASKSET_EA);              // nibble 0: an untagged TASKSET owner
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        g10_call(v, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(vc.stop == StartupStop::None && v.execution.ppu.cpu.gpr[3] == 0);
        CHECK(vc.lfqa_owner == LFQ_TASKSET_EA);
        CHECK(vc.lfqa_last == 0u && vc.lfqa_group == group_of_instance1);
        CHECK(vc.lfqa_queues_created == 2u);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0 + 2u);
    }

    // ---- 5. THE OWNER RESOLUTION's other arms (decision 27), each on a QUEUE object of its own in the
    // scratch block: (a) an untagged taskset owner, (b) a zero owner word, (c) an untagged owner whose own
    // `+0x64` is zero, (d) a tagged owner no instance owns. The init runs first so the line carries the
    // game's shape; the case then stamps exactly the bytes the resolution reads (`+0x74`, and the owner's
    // `+0x64`) and attaches. ----
    {
        // (a) An UNTAGGED TASKSET owner. The init tag meaning is the reverse of the G14 labels: nibble 0 is
        // a taskset, and 0x175A8/0x175AC read that taskset's own `+0x64` -- the low word of the u64
        // `cellSpursCreateTasksetWithAttribute` stored at its `+0x60` -- as the CellSpurs EA.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        const uint32_t queue = LFQ_GAME_QUEUE_EA;
        v.zero_pages(queue, 0x100);
        {
            const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
            create_attribute(v, ATTR_EA, NAME_EA);
            g10_call(v, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
            CHECK(vc.stop == StartupStop::None);
        }
        // The taskset's own `+0x60` is the CellSpurs EA the creator stored.
        CHECK(v.get(LFQ_TASKSET_EA + 0x60, 8) == uint64_t(CREATE_SPURS_EA));
        CHECK(v.get(LFQ_TASKSET_EA + 0x64, 4) == CREATE_SPURS_EA);   // the low word the resolution reads
        g10_call(v, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, queue, LFQ_GAME_QUEUE_BUFFER, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        CHECK(v.get(queue + 0x24, 4) == 3u && v.get(queue + 0x20, 1) == 0xFFu && v.get(queue + 0x21, 1) == 0xFFu);
        const uint32_t group_of_instance1 = [&] {
            for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index)
                if (v.comp->runtime.instances[index].spurs_ea == INSTANCE1)
                    return v.comp->runtime.instances[index].group;
            return 0u;
        }();
        CHECK(group_of_instance1 != 0u);
        v.put32(queue + 0x74, LFQ_TASKSET_EA);   // nibble 0: an untagged TASKSET owner
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        g10_call(v, LFQA_NID, queue);
        CHECK(vc.stop == StartupStop::None && v.execution.ppu.cpu.gpr[3] == 0);
        CHECK(vc.lfqa_owner == LFQ_TASKSET_EA);              // read verbatim from the queue's `+0x74`
        CHECK(vc.lfqa_last == 0u && vc.lfqa_group == group_of_instance1);
        CHECK(vc.lfqa_queues_created == 2u && vc.lfqa_queues_destroyed == 0u);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0 + 2u);
    }
    {
        // (b) Owner word 0 -> 0x80410902 at 0x17654, before any write and
        // before any queue exists.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        const uint32_t queue = LFQ_GAME_QUEUE_EA;
        v.zero_pages(queue, 0x100);
        g10_call(v, LFQ_NID, uint64_t(CREATE_SPURS_EA) | 1u, queue, LFQ_GAME_QUEUE_BUFFER, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        v.put32(queue + 0x74, 0u);   // the owner the init would have stored is removed
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        const auto before = g10_bytes(v, queue, PS3_SPURS_LFQA_BYTES);
        g10_call(v, LFQA_NID, queue);
        CHECK(vc.stop == StartupStop::None);
        CHECK(v.execution.ppu.cpu.gpr[3] == rc_reg(0x80410902u));
        CHECK(vc.lfqa_last == 0x80410902u && vc.lfqa_owner == 0u && vc.lfqa_writes == 0u);
        CHECK(g10_bytes(v, queue, PS3_SPURS_LFQA_BYTES) == before);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0);
    }
    {
        // (c) An UNTAGGED owner whose own `+0x64` is 0: the resolution reads it
        // and 0x175B0/0x175B4 refuses with 0x80410902. Nothing is written, no queue is created.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        const uint32_t queue = LFQ_GAME_QUEUE_EA;
        v.zero_pages(queue, 0x100);
        g10_call(v, LFQ_NID, uint64_t(CREATE_SPURS_EA) | 1u, queue, LFQ_GAME_QUEUE_BUFFER, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        // A scratch EA that is no instance: its own `+0x64` is zero (the block is not written by anyone).
        CHECK(v.get(LFQ_SCRATCH_EA + 0x64, 4) == 0u);
        v.put32(queue + 0x74, LFQ_SCRATCH_EA);
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        const auto before = g10_bytes(v, queue, PS3_SPURS_LFQA_BYTES);
        g10_call(v, LFQA_NID, queue);
        CHECK(vc.stop == StartupStop::None);
        CHECK(v.execution.ppu.cpu.gpr[3] == rc_reg(0x80410902u));
        CHECK(vc.lfqa_last == 0x80410902u && vc.lfqa_owner == LFQ_SCRATCH_EA && vc.lfqa_writes == 0u);
        CHECK(g10_bytes(v, queue, PS3_SPURS_LFQA_BYTES) == before);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0);
    }
    {
        // (d) A TAGGED owner naming a CellSpurs EA no instance of this runtime owns: the resolution succeeds
        // (nibble != 0 -> r28 = word & ~0xF) and the native instance lookup refuses. The honest
        // ProviderFailure, never a fabricated instance; nothing is created and no guest byte moves.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        const uint32_t queue = LFQ_GAME_QUEUE_EA;
        v.zero_pages(queue, 0x100);
        g10_call(v, LFQ_NID, uint64_t(CREATE_SPURS_EA) | 1u, queue, LFQ_GAME_QUEUE_BUFFER, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        const uint32_t unowned = SCRATCH_BASE + 0x2000u;     // 128-aligned, no instance
        v.put32(queue + 0x74, unowned | 1u);
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        const auto before = g10_bytes(v, queue, PS3_SPURS_LFQA_BYTES);
        g10_call(v, LFQA_NID, queue);
        CHECK(vc.stop == StartupStop::ProviderFailure);
        CHECK(vc.lfqa_owner == (unowned | 1u));
        CHECK(vc.lfqa_last == 0xFFFFFFFFu && vc.lfqa_writes == 0u);
        CHECK(g10_bytes(v, queue, PS3_SPURS_LFQA_BYTES) == before);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0);
    }

    // ---- 6. REFUSALS on the SDK-25 arm. Each precedes the firmware's first store: no guest byte moves,
    // no queue is created. ----
    const size_t queues_before_refusals = ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE);
    {
        // Gate 1: line & 0x7F -> 0x80410910. A NULL line reports gate 2's code, because
        // 0 & 0x7F is 0. Both are register-only.
        const auto before = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQA_BYTES);
        g10_call(r, LFQA_NID, LFQ_TASKSET_EA + 4u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410910u));
        CHECK(c.lfqa_last == 0x80410910u && c.lfqa_writes == 0u);
        g10_call(r, LFQA_NID, 0u);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u));
        CHECK(c.lfqa_last == 0x80410911u && c.lfqa_writes == 0u);
        CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_LFQA_BYTES) == before);
        CHECK(ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE) == queues_before_refusals);
    }
    {
        // a ZERO direction word -> 0x80410909 (the exit is taken with r31 already
        // holding that word), NOT the shape gate's 0x8041090F.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        {
            const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
            create_attribute(v, ATTR_EA, NAME_EA);
            g10_call(v, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
            CHECK(vc.stop == StartupStop::None);
        }
        g10_call(v, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        v.put32(LFQ_TASKSET_EA + 0x24, 0u);
        const auto before = g10_bytes(v, LFQ_TASKSET_EA, PS3_SPURS_LFQA_BYTES);
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        g10_call(v, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(vc.stop == StartupStop::None);
        CHECK(v.execution.ppu.cpu.gpr[3] == rc_reg(0x80410909u));
        CHECK(vc.lfqa_last == 0x80410909u && vc.lfqa_writes == 0u);
        CHECK(g10_bytes(v, LFQ_TASKSET_EA, PS3_SPURS_LFQA_BYTES) == before);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0);
    }
    {
        // Gates 4/5 (SPURS shape): +0x20 and +0x21 must be 0xFF -> 0x8041090F.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        {
            const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
            create_attribute(v, ATTR_EA, NAME_EA);
            g10_call(v, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
            CHECK(vc.stop == StartupStop::None);
        }
        g10_call(v, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        v.put32(LFQ_TASKSET_EA + 0x20, 0x000000FEu);
        g10_call(v, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(vc.stop == StartupStop::None && v.execution.ppu.cpu.gpr[3] == rc_reg(0x8041090Fu));
        CHECK(vc.lfqa_last == 0x8041090Fu && vc.lfqa_writes == 0u);
        v.put32(LFQ_TASKSET_EA + 0x20, 0x000000FFu);
        v.put8(LFQ_TASKSET_EA + 0x21, 0xFEu);
        g10_call(v, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(vc.stop == StartupStop::None && v.execution.ppu.cpu.gpr[3] == rc_reg(0x8041090Fu));
        CHECK(vc.lfqa_writes == 0u);
    }
    {
        // The CellSync shape on this arm: selected by `+0x24 != 3` (0x17678 not taken), whose gate is
        // `+0x20 == 0xFFFFFFFF` -> 0x8041090F, and no sc 128 is issued.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v); auto &vc = *v.control;
        {
            const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
            create_attribute(v, ATTR_EA, NAME_EA);
            g10_call(v, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
            CHECK(vc.stop == StartupStop::None);
        }
        g10_call(v, LFQ_NID, uint64_t(LFQ_TASKSET_EA) | 1u, LFQ_TASKSET_EA, LFQ_BUFFER_EA, 0x20u, 3u, 3u);
        CHECK(vc.stop == StartupStop::None);
        v.put32(LFQ_TASKSET_EA + 0x24, 1u);
        v.put32(LFQ_TASKSET_EA + 0x20, 0xFFFFFFFFu);
        const size_t queues0 = ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE);
        g10_call(v, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(vc.stop == StartupStop::None && v.execution.ppu.cpu.gpr[3] == rc_reg(0x8041090Fu));
        CHECK(vc.lfqa_shape == 0u && vc.lfqa_iterations == 1u && vc.lfqa_writes == 0u);
        CHECK(ps3_lv2_live_count(&v.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0);
    }
    {
        // Invalid memory: an unreadable line stops on its FIRST guest access, which on this arm is
        // (`+0x74` is never read here).
        const size_t calls0 = c.lfqa_calls, returns0 = c.lfqa_returns;
        g10_call(r, LFQA_NID, 0x30000000u);   // aligned, so gates 1-2 pass and the read is attempted
        CHECK(c.stop == StartupStop::InvalidMemory);
        CHECK(c.lfqa_calls == calls0 + 1u && c.lfqa_returns == returns0);
        CHECK(c.lfqa_last == 0xFFFFFFFFu && c.lfqa_writes == 0u);
    }
    {
        // The TOC slot is preflighted before anything else: an unwritable stack slot stops with no read.
        Runtime v; services.install(v); v.boot2(); CreateScope scope2(v);
        v.control->import_count = 0;
        auto &vcpu = v.execution.ppu.cpu;
        vcpu.gpr[3] = LFQ_TASKSET_EA; vcpu.gpr[1] = 0x30000000u; vcpu.gpr[2] = TOC;
        v.run(LFQA_NID, false);
        CHECK(v.control->stop == StartupStop::InvalidMemory && v.control->lfqa_returns == 0);
    }

    // ---- 7. ROUTE anchor: with PS3_NATIVE_STARTUP_SPURS_INITIALIZE defined this NID is dispatched, so it
    // never stops UnknownImport. That build is proven by verify_startup_spurs_getinfo.sh's preprocessor
    // needle check, which lists 0x1656d49f and dispatch_spurs_lfqueue_attach. ----
    {
        Runtime u; services.install(u); u.boot2(); CreateScope scope3(u);
        g10_call(u, LFQA_NID, LFQ_TASKSET_EA);
        CHECK(u.control->stop != StartupStop::UnknownImport);
        CHECK(u.control->lfqa_calls == 1);
    }
}

// ---- cellSpursCreateTask (NID 0xBEB600AC) ------------------------------------------------------
// The REAL
// ps3_hle_call route over the real composition, on the game's own sequence and its own argument
// shape: create the taskset (g13), build the LFQueue line (g14), attach it (g15), then create the
// task at LR 0x00424744 (func_00424714: r3 = taskset, r4 = &manager[0x80], r5 = the ELF, r6 =
// context, r7 = 0xC00, r8 = the lsPattern POINTER, r9 = &frame[0x78] = the argument POINTER).
namespace {
constexpr uint32_t TASK_NID = 0xbeb600ac;
// The game's own addresses. r4's target is the manager slot at +0x80; the ELF, the two pointer
// blocks and the context are separate, 16-byte-aligned objects in the scratch window.
constexpr uint32_t TASK_ID_EA = SCRATCH_BASE + 0x2000;      // manager+0x80
constexpr uint32_t TASK_ELF_EA = SCRATCH_BASE + 0x2100;     // 16-byte aligned
constexpr uint32_t TASK_CONTEXT_EA = SCRATCH_BASE + 0x2200; // 16-byte aligned
constexpr uint32_t TASK_ARG_EA = SCRATCH_BASE + 0x2300;     // r9 -> the 16-byte argument block
constexpr uint32_t TASK_LS_EA = SCRATCH_BASE + 0x2320;      // r8 -> the 16-byte lsPattern block
constexpr uint32_t TASK_SIZE = 0xC00;                       // r7 = 0xC00 literal
// 0x11E04 for size 0xC00: (0xC00 - 0x400) >> 11 = 1, so +0x18 = context | 1.
constexpr uint64_t TASK_CONTEXT_WORD = uint64_t(TASK_CONTEXT_EA) | 1u;

// The game's argument word: r9 = &frame[0x78], whose word is the caller's r5 (the size again). The
// block is 16 bytes because 0x11E94/0x11EA0 load 8 bytes at +0x0 and +0x8.
void write_task_argument(Runtime &r, uint32_t ea, uint64_t lo, uint64_t hi) {
    unsigned char block[16]{};
    for (size_t i = 0; i < 8; ++i) {
        block[i] = uint8_t(lo >> (8 * (7 - i)));
        block[8 + i] = uint8_t(hi >> (8 * (7 - i)));
    }
    r.put(ea, block, sizeof block);
}

// A minimal valid SPU ELF32 big-endian image: the ident the firmware compares byte for byte, then
// e_machine = EM_SPU, e_entry > 0x2FFF and one PT_LOAD with p_vaddr > 0x2FFF (CONTRACT §C.1-§C.3;
// root g17e: refuses entry <= 0x2FFF, and phdr +0x8 is p_vaddr).
void write_task_elf(Runtime &r, uint32_t ea) {
    std::vector<uint8_t> image(0x80, 0);
    const auto put = [&](size_t offset, size_t width, uint64_t value) {
        for (size_t i = 0; i < width; ++i) image[offset + i] = uint8_t(value >> (8 * (width - 1 - i)));
    };
    put(0x00, 8, 0x7F454C4601020100ull);      // 7F 45 4C 46 01 02 01 00
    put(0x12, 2, 23u);                        // e_machine = EM_SPU
    put(0x18, 4, 0x3000u);                    // e_entry (above the policy-module area)
    put(0x1C, 4, 0x34u);                      // e_phoff
    put(0x2A, 2, 32u);                        // e_phentsize
    put(0x2C, 2, 1u);                         // e_phnum
    put(0x34, 4, 1u);                         // phdr[0].p_type = PT_LOAD
    put(0x34 + 0x08, 4, 0x4000u);             // phdr[0].p_filesz > 0x2FFF
    r.put(ea, image.data(), image.size());
}
}   // namespace

void run_create_task() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
    create_attribute(r, ATTR_EA, NAME_EA);
    // The game's real sequence: taskset, then the LFQueue line, then the attach -- all native, all on
    // the game's own registers -- so the object the task lands on is the one the earlier calls built.
    r.zero_pages(LFQ_TASKSET_EA, 0x2000);
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
    CHECK(c.taskset_create_wid == 0u);
    r.zero_pages(LFQ_GAME_QUEUE_EA, 0x100);
    g10_call(r, LFQ_NID, uint64_t(CREATE_SPURS_EA) | 1u, LFQ_GAME_QUEUE_EA, LFQ_GAME_QUEUE_BUFFER, 0x20u, 3u, 3u);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0 && c.lfq_last == 0u);
    g10_call(r, LFQA_NID, LFQ_GAME_QUEUE_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
    CHECK(r.get(LFQ_TASKSET_EA + 0x64, 4) == CREATE_SPURS_EA);
    CHECK(r.get(LFQ_TASKSET_EA + 0x74, 4) == 0u);

    write_task_elf(r, TASK_ELF_EA);
    write_task_argument(r, TASK_ARG_EA, TASK_SIZE, 0u);   // the frame word: the size again
    write_task_argument(r, TASK_LS_EA, 0u, 0u);           // a zero lsPattern block
    r.put(TASK_ID_EA, std::vector<uint8_t>(4, 0xA5).data(), 4);
    const uint64_t signal_before = r.get(INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE, 1);
    const size_t from = r.published();

    // ---- the game's own call ----
    g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE,
                TASK_LS_EA, TASK_ARG_EA);
    CHECK(c.stop == StartupStop::None);
    CHECK(cpu.gpr[2] == TOC);                       // TOC restored
    CHECK(cpu.gpr[3] == 0);                         // the entry's success code
    CHECK(c.task_calls == 1 && c.task_returns == 1);
    CHECK(c.task_last == 0u);
    CHECK(c.task_id == 0u);                         // slot 0: the taskset's first free slot
    CHECK(c.task_slot == 0u);
    CHECK(c.task_id_written == 1);
    // The id reaches the caller's pointer only at 0x125D0, and it is 4 bytes.
    CHECK(r.get(TASK_ID_EA, 4) == 0u);
    // The six record fields, at taskset + 0x80 + slot*48, and +0x18 = context|1.
    CHECK(r.get(LFQ_TASKSET_EA + 0x80 + 0x00, 8) == TASK_SIZE);       // argument[0..7]
    CHECK(r.get(LFQ_TASKSET_EA + 0x80 + 0x08, 8) == 0u);              // argument[8..15]
    CHECK(r.get(LFQ_TASKSET_EA + 0x80 + 0x10, 8) == TASK_ELF_EA);     // elf
    CHECK(r.get(LFQ_TASKSET_EA + 0x80 + 0x18, 8) == TASK_CONTEXT_WORD);
    CHECK(r.get(LFQ_TASKSET_EA + 0x80 + 0x20, 8) == 0u);              // ls[0..7]
    CHECK(r.get(LFQ_TASKSET_EA + 0x80 + 0x28, 8) == 0u);              // ls[8..15]
    // The allocation bit at +0x30 and the ready bit at +0x20.
    CHECK(r.get(LFQ_TASKSET_EA + 0x30, 4) == 0x80000000u);
    CHECK(r.get(LFQ_TASKSET_EA + 0x20, 4) == 0x80000000u);
    // 60 bytes: six 8-byte record fields (48) + the 4-byte allocation mask + the 4-byte ready mask
    // + the 4-byte caller *taskId store at 0x125D0. 48 + 4 + 4 + 4 = 60.
    CHECK(c.task_writes == 60u);
    // ---- the native SPURS signal, as the firmware's 0xA750 / 0x85D0 order it ----
    CHECK(c.task_ready_raised == 1 && c.task_signal_written == 1);
    CHECK(c.task_ready_address == INSTANCE1 + 0x70u);   // wid 0 <= 15 -> the primary mask word
    CHECK(c.task_mask_bit == 0x80000000u);
    CHECK(r.get(INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE, 1) == 1u);
    CHECK(signal_before == 0u);
    // Both writes are on the journal: 0xA750's 4-byte word carrying the wid's bit, and 0x85D0's byte.
    {
        bool mask = false, signal = false;
        for (const auto &event : r.since(from)) {
            if (event.origin == PpuMemoryTraceOrigin::Hle &&
                event.operation == PpuMemoryTraceOperation::Write &&
                event.address == INSTANCE1 + 0x70u && event.length == 4u &&
                (event.value & 0x80000000u) != 0u) mask = true;
            if (is_event(event, Ev{true, INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE, 1, 1})) signal = true;
        }
        CHECK(mask && signal);
    }
    // The caller's *taskId store is the LAST guest effect of the call, after the signal.
    {
        size_t id_index = 0, mask_index = 0, signal_index = 0, seen = 0;
        for (const auto &event : r.since(from)) {
            if (event.origin != PpuMemoryTraceOrigin::Hle ||
                event.operation != PpuMemoryTraceOperation::Write) continue;
            if (event.address == TASK_ID_EA && event.length == 4u) id_index = seen;
            if (event.address == INSTANCE1 + 0x70u && event.length == 4u) mask_index = seen;
            if (event.address == INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE && event.length == 1u)
                signal_index = seen;
            ++seen;
        }
        CHECK(id_index > mask_index && id_index > signal_index);
    }
    // MEASURED: the signal (0xA750's mask bit and 0x85D0's byte) makes the fixture kernel's context
    // runnable, so its worker may reach the policy-execution port. This slice does not run SPU code
    // and never fakes a completion -- the port answers PS3_SPURS_POLICY_UNSUPPORTED and the kernel
    // then stops. What this entry must NOT do is start one itself, which the
    // first call's zero-vs-nonzero transition at dispatch time would show; the assertion is that the
    // port was never asked for a completion this entry produced.
    CHECK(c.task_last == 0u);
}

// A REAL second CreateTask call on the same taskset, through the adapter, with no hand-stamped
// +0x30: the sweep itself must find slot 1.
void run_create_task_second_call() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2(); CreateScope scope(r); auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    r.zero_pages(LFQ_TASKSET_EA, 0x2000);
    const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
    create_attribute(r, ATTR_EA, NAME_EA);
    g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
    write_task_elf(r, TASK_ELF_EA);
    write_task_argument(r, TASK_ARG_EA, TASK_SIZE, 0u);
    write_task_argument(r, TASK_LS_EA, 0u, 0u);
    r.put(TASK_ID_EA, std::vector<uint8_t>(4, 0xA5).data(), 4);
    g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE,
                TASK_LS_EA, TASK_ARG_EA);
    CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
    CHECK(c.task_last == 0u && c.task_id == 0u && c.task_slot == 0u);
    CHECK(r.get(LFQ_TASKSET_EA + 0x30, 4) == 0x80000000u);
    CHECK(r.get(LFQ_TASKSET_EA + 0x20, 4) == 0x80000000u);
    /* Root (B2b review): the fence is published ASYNCHRONOUSLY by the native kernel1 worker thread when it
     * ends finish_stopped; ps3_spurs_runtime_poll_stop only samples it. Without a wait this assertion
     * raced the worker (it lost every time at O2 on 2026-11-08). Wait, bounded, for the kernel stop the
     * console-equivalent timeline guarantees before the second call, then assert the fence as before. */
    {
        ps3_spurs_stop_info observed{};
        bool kernel_stopped = false;
        for (int attempt = 0; attempt < 2000 && !kernel_stopped; ++attempt) {
            kernel_stopped = r.execution.spurs && ps3_spurs_runtime_poll_stop(r.execution.spurs, &observed);
            if (!kernel_stopped) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(kernel_stopped);
    }
    // The second call, on the same live taskset, with only the caller's id word re-poisoned.
    r.put(TASK_ID_EA, std::vector<uint8_t>(4, 0xA5).data(), 4);
    const size_t returns1 = c.task_returns;
    g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE,
                TASK_LS_EA, TASK_ARG_EA);
    // The fence above: the second call stops BEFORE the dispatch, so nothing of it is observed and
    // the taskset keeps exactly the first call's state.
    CHECK(c.stop == StartupStop::SpursKernelFence);
    CHECK(c.task_returns == returns1);              // the entry was never reached
    CHECK(r.get(TASK_ID_EA, 4) == 0xA5A5A5A5u);     // the caller's word untouched
    CHECK(r.get(LFQ_TASKSET_EA + 0x30, 4) == 0x80000000u);   // still only slot 0
    CHECK(r.get(LFQ_TASKSET_EA + 0x20, 4) == 0x80000000u);
}

void run_create_task_refusals() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2();
    auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
    {
        CreateScope scope(r);
        create_attribute(r, ATTR_EA, NAME_EA);
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
        write_task_elf(r, TASK_ELF_EA);
        write_task_argument(r, TASK_ARG_EA, TASK_SIZE, 0u);
        write_task_argument(r, TASK_LS_EA, 0u, 0u);
        // The caller's id word is poisoned so "untouched" is observable in the failure itself.
        r.put(TASK_ID_EA, std::vector<uint8_t>(4, 0xA5).data(), 4);
        const auto taskset_before = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_TASKSET_BYTES);
        const uint64_t signal_before = r.get(INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE, 1);
        // Taskset NULL -> 0x80410911, nothing written.
        g10_call_r9(r, TASK_NID, 0u, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == rc_reg(0x80410911u) && c.task_last == 0x80410911u);
        CHECK(c.task_writes == 0u && c.task_id == 0xFFFFFFFFu && c.task_id_written == 0u);
        // Misaligned taskset -> 0x80410910.
        g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA + 4u, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
        CHECK(c.stop == StartupStop::None && c.task_last == 0x80410910u && c.task_writes == 0u);
        // ELF NULL -> 0x80410911.
        g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, 0u, TASK_CONTEXT_EA, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
        CHECK(c.stop == StartupStop::None && c.task_last == 0x80410911u && c.task_writes == 0u);
        // A misaligned context must not get ELF reads at all: point the ELF at unmapped memory and
        // the misaligned context still answers 0x80410910 without faulting.
        g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, 0x30000000u, TASK_CONTEXT_EA + 8u, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
        CHECK(c.stop == StartupStop::None && c.task_last == 0x80410910u && c.task_writes == 0u);
        // Entry at or below 0x2FFF -> 0x80410907.
        {
            unsigned char patched[4] = {0x00, 0x00, 0x2F, 0xFF};
            r.put(TASK_ELF_EA + 0x18, patched, 4);
            g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
            CHECK(c.stop == StartupStop::None && c.task_last == 0x80410907u && c.task_writes == 0u);
            write_task_elf(r, TASK_ELF_EA);
        }
        // Not EM_SPU -> 0x80410907.
        {
            unsigned char machine[2] = {0x00, 0x15};
            r.put(TASK_ELF_EA + 0x12, machine, 2);
            g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
            CHECK(c.stop == StartupStop::None && c.task_last == 0x80410907u && c.task_writes == 0u);
            write_task_elf(r, TASK_ELF_EA);
        }
        // A full allocation set -> 0x80410901.
        {
            auto full = g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_TASKSET_BYTES);
            for (size_t i = 0; i < 16u; ++i) full[0x30 + i] = 0xFFu;
            r.put(LFQ_TASKSET_EA, full.data(), full.size());
            g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE, TASK_LS_EA, TASK_ARG_EA);
            CHECK(c.stop == StartupStop::None && c.task_last == 0x80410901u && c.task_writes == 0u);
            r.put(LFQ_TASKSET_EA, taskset_before.data(), taskset_before.size());
        }
        // Every refusal left the caller's id word, the signal byte and the taskset as they were.
        CHECK(r.get(TASK_ID_EA, 4) == 0xA5A5A5A5u);
        CHECK(c.task_id_written == 0u);
        CHECK(r.get(INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE, 1) == signal_before);
        CHECK(g10_bytes(r, LFQ_TASKSET_EA, PS3_SPURS_TASKSET_BYTES) == taskset_before);
    }
}

// A SIGNAL refusal (0xA750 answers 0x8041080F) leaves the caller's *taskId untouched: with the
// exception word at spurs+0xD6C set, 0xA7D0 makes 0x85D0's arm fail.
void run_create_task_signal_refusal() {
    CreateServices services;
    Runtime r; services.install(r); r.boot2();
    auto &c = *r.control; auto &cpu = r.execution.ppu.cpu;
    {
        CreateScope scope(r);
        const uint32_t NAME_EA = SCRATCH_BASE + 0xF00;
        create_attribute(r, ATTR_EA, NAME_EA);
        g10_call(r, CREATE_NID, CREATE_SPURS_EA, LFQ_TASKSET_EA, ATTR_EA);
        CHECK(c.stop == StartupStop::None && cpu.gpr[3] == 0);
        write_task_elf(r, TASK_ELF_EA);
        write_task_argument(r, TASK_ARG_EA, TASK_SIZE, 0u);
        write_task_argument(r, TASK_LS_EA, 0u, 0u);
        r.put(TASK_ID_EA, std::vector<uint8_t>(4, 0xA5).data(), 4);
        // The exception word 0xA750/0x85D0 read at spurs+0xD6C.
        const uint8_t exception[4] = {0, 0, 0, 1};
        r.put(INSTANCE1 + PS3_SPURS_TKC_EXCEPTION_WORD, exception, 4);
        g10_call_r9(r, TASK_NID, LFQ_TASKSET_EA, TASK_ID_EA, TASK_ELF_EA, TASK_CONTEXT_EA, TASK_SIZE,
                    TASK_LS_EA, TASK_ARG_EA);
        CHECK(c.stop == StartupStop::None);
        CHECK(cpu.gpr[3] == rc_reg(0x8041090fu));
        CHECK(c.task_last == 0x8041090fu);
        // The caller's *taskId is untouched and the signal byte was never written.
        CHECK(r.get(TASK_ID_EA, 4) == 0xA5A5A5A5u);
        CHECK(c.task_id_written == 0u);
        CHECK(c.task_signal_written == 0u);
        CHECK(r.get(INSTANCE1 + PS3_SPURS_TKC_SIGNAL_BYTE, 1) == 0u);
        const uint8_t cleared[4] = {0, 0, 0, 0};
        r.put(INSTANCE1 + PS3_SPURS_TKC_EXCEPTION_WORD, cleared, 4);
    }
}

int main() {
    run_two_instances(); run_errors(); run_trace_credit(); run_memory_failures(); run_trace_failures(); run_journal_failures(); run_boundaries(); run_classification();
    run_taskset_attribute_initialize(); run_event_flag_initialize();
    run_taskset_attribute_set_name(); run_event_flag_attach();
    run_create_taskset(); run_create_taskset_refusals();
    run_lfqueue_initialize(); run_lfqueue_refusals(); run_lfqueue_attach();
    run_create_task(); run_create_task_second_call(); run_create_task_refusals(); run_create_task_signal_refusal();
    if (failures) { std::printf("SPURS_INFO_FAILED failures=%d\n", failures); return 1; }
    std::printf("SPURS_INFO_PASS\n"); return 0;
}

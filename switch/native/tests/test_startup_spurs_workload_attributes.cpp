// Adapter integration for _cellSpursWorkloadAttributeInitialize (0xefeb2679) and cellSpursWorkloadAttributeSetName (0x4a5eab63).
// Real collaborators: production startup adapter, PPU memory binding, atomic/coherence domain, trace recorder/journal, pure
// workload attribute domain. Test doubles are limited to the host-backed guest window (fault/cancel injection) and the clock.
// Every expectation below is an independent literal, never produced by the encoder under test.
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>
#include <initializer_list>

namespace {
int failures, checks;
#define CHECK(value) do { ++checks; if (!(value)) { std::printf("WORKLOAD_ATTR_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
pthread_mutex_t *terminal_guard;
uint64_t g_tick;
void lock(void *p) { if (pthread_mutex_lock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
void unlock(void *p) { if (pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
[[maybe_unused]] constexpr uint32_t BASE = 0xd00ff000, SP = 0xd00ff910, TOC_SLOT = 0xd00ff938, STACK_SLOT = 0xd00ff980;
[[maybe_unused]] constexpr uint32_t ATTR = 0xd00ff990, PRIO = 0xd0100100, PM = 0x69c880, UNMAPPED = 0xd0101000;
[[maybe_unused]] constexpr uint32_t INIT_NID = 0xefeb2679, NAME_NID = 0x4a5eab63;
[[maybe_unused]] constexpr uint64_t TOC = UINT64_C(0x123456789abcdef0);
[[maybe_unused]] constexpr uint64_t SDK_NULL = UINT64_C(0xffffffff80410811), SDK_ALIGN = UINT64_C(0xffffffff80410810), SDK_INVAL = UINT64_C(0xffffffff80410802);
// Independent literal of the first 0x28 bytes for 1/0x220002/0x69c880/0x2d80/0x404e9300/prio 00 01 02 0f 08 00 0e 07/min 1/max 5.
const unsigned char kHead[0x28] = {0,0,0,1, 0,0x22,0,2, 0,0x69,0xc8,0x80, 0,0,0x2d,0x80, 0,0,0,0,0x40,0x4e,0x93,0,
    0,1,2,0x0f,8,0,0x0e,7, 0,0,0,1, 0,0,0,5};
const unsigned char kPriority[8] = {0,1,2,0x0f,8,0,0x0e,7};
uint64_t be(const unsigned char *p, size_t n) { uint64_t v = 0; for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i]; return v; }
struct Range { uint32_t address; size_t length; };

// Host-backed guest window: every read/write that reaches the provider is recorded byte-exactly; faults, cancellation, deadline
// and trace-journal exhaustion are injected only at the exact provider operation named by the test.
struct GuestWindow {
    static constexpr uint32_t base = 0xd00ff000;
    static constexpr size_t span = 0x3000;
    ElfTestHostSpace *space = elf_test_host_space_create(span);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    StartupControl *control = nullptr;
    unsigned char rmask[span]{}, wmask[span]{};
    size_t reads = 0, writes = 0;
    uint32_t fail_read = 0, cancel_read = 0, fail_write = 0, cancel_write = 0, partial_write = 0, shrink_journal = 0, tick_write = 0;
    static bool contains(uint32_t a, uint64_t n) { return a >= base && uint64_t(a) - base <= span && n <= span - (uint64_t(a) - base); }
    static void mark(unsigned char *mask, uint32_t a, size_t n) { if (contains(a, n)) std::memset(mask + (a - base), 1, n); }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.commit(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.decommit(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status read(void *p, uint32_t a, void *b, size_t n) {
        auto &s = *static_cast<GuestWindow *>(p); ++s.reads;
        if (s.fail_read && a == s.fail_read) return PS3_GUEST_MEMORY_UNMAPPED;
        const auto status = contains(a, n) ? s.backing.read(s.backing.context, a - base, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
        if (status == PS3_GUEST_MEMORY_OK) mark(s.rmask, a, n);
        if (s.cancel_read && a == s.cancel_read) s.control->cancelled.store(true);
        return status;
    }
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *b, size_t n) {
        auto &s = *static_cast<GuestWindow *>(p); ++s.writes;
        if (s.fail_write && a == s.fail_write) return PS3_GUEST_MEMORY_UNMAPPED;
        if (s.partial_write && a == s.partial_write) {   // two bytes really land, then the provider reports failure
            CHECK(contains(a, n) && n >= 2);
            CHECK(s.backing.write(s.backing.context, a - base, b, 2) == PS3_GUEST_MEMORY_OK);
            mark(s.wmask, a, 2);
            return PS3_GUEST_MEMORY_UNMAPPED;
        }
        const auto status = contains(a, n) ? s.backing.write(s.backing.context, a - base, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
        if (status == PS3_GUEST_MEMORY_OK) mark(s.wmask, a, n);
        if (s.cancel_write && a == s.cancel_write) s.control->cancelled.store(true);
        if (s.tick_write && a == s.tick_write) g_tick = 100;
        if (s.shrink_journal && a == s.shrink_journal) s.control->journal.capacity = s.control->journal.count;
        return status;
    }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.zero(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
    ~GuestWindow() { elf_test_host_space_destroy(space); }
};
struct Args {   // r3..r10
    uint64_t r[8] = {ATTR, 1, 0x220002, PM, 0x2d80, UINT64_C(0x404e9300), PRIO, 1};
};
struct Options {
    size_t trace = 128;          // recorder capacity (events)
    bool journal = false;        // legacy journal instead of the recorder
    uint64_t ram = 8192;         // permitted RAM length (the coherent domain commits only 8192)
    bool readonly_attr = false;  // [ATTR, ATTR+512) is read-only
};
struct Runtime {
    Options options;
    GuestWindow window;
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[2]{};
    ps3_atomic_pin_record pins[2]{};
    PpuMemoryRamRange ram;
    PpuMemoryRamRange partial_permissions[3];
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[128]{};
    ps3_clock_status sample_status = PS3_CLOCK_OK;
    static ps3_clock_status sample(void *, uint64_t *value) { *value = 1; return PS3_CLOCK_OK; }
    ps3_clock clock{this, sample};
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    ps3_atomic_domain &initialize() {
        if (!window.space || ps3_atomic_domain_init(&domain, window.port(), {&guard, lock, unlock}, owners, 2, pins, 2) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    unsigned char before_memory[8192]{}, after_memory[8192]{};
    size_t event_begin = 0, event_end = 0;
    StartupStop stop = StartupStop::None;
    explicit Runtime(Options o = Options())
        : options(o), ram{GuestWindow::base, o.ram, true},
          partial_permissions{{GuestWindow::base, 0x990, true}, {GuestWindow::base + 0x990, 512, false},
              {GuestWindow::base + 0xb90, o.ram - 0xb90, true}},
          binding(initialize(), o.readonly_attr ? partial_permissions : &ram, o.readonly_attr ? 3 : 1),
          policy({64, 64, 64, UINT64_C(1000000000)}, 0), budget(policy, clock) {
        g_tick = 0;
        CHECK(ps3_atomic_commit(&domain, GuestWindow::base, 8192) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        execution.control = control.get(); window.control = control.get();
        execution.guest_priority = 250; execution.has_guest_priority = true;
        execution.ppu.cpu.thread_id = 1;
        execution.ppu.cpu.gpr[1] = SP;
        execution.ppu.cpu.gpr[2] = TOC;
        execution.ppu.cpu.lr = 0x2cfe70;
        if (o.journal) {
            CHECK(native_startup_admit(execution, binding, services));
        } else {
            CHECK(recorder.configure(events, o.trace, PpuMemoryTraceRetention::Strict, 0));
            CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
        }
        unsigned char bytes[8192]; std::memset(bytes, 0xa5, sizeof(bytes));
        seed(GuestWindow::base, bytes, sizeof(bytes));
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(!pthread_mutex_destroy(&guard));
    }
    ppu_context &cpu() { return execution.ppu.cpu; }
    StartupControl &c() { return *control; }
    void seed(uint32_t address, const void *bytes, size_t length) {
        CHECK(binding.coherent_memory.write(binding.coherent_memory.context, address, bytes, length) == PS3_GUEST_MEMORY_OK);
    }
    void seed_be64(uint32_t address, uint64_t value) {
        unsigned char b[8]; for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(value >> (56 - 8 * i));
        seed(address, b, 8);
    }
    void seed_actual() { seed_be64(STACK_SLOT, 5); seed(PRIO, kPriority, 8); }
    size_t event_count() { return options.journal ? control->journal.count : recorder.report().published_count; }
    const PpuMemoryTraceEvent &event(size_t i) { return options.journal ? control->journal.events[i] : events[i]; }
    size_t n_events() const { return event_end - event_begin; }
    bool ev(size_t i, bool write, uint32_t a, size_t len, uint64_t v) {
        if (event_begin + i >= event_end) return false;
        const auto &e = event(event_begin + i);
        return e.origin == PpuMemoryTraceOrigin::Hle && e.operation == (write ? PpuMemoryTraceOperation::Write : PpuMemoryTraceOperation::Read) &&
            e.address == a && e.length == len && e.value == v && !e.byte_transfer && e.status == PS3_GUEST_MEMORY_OK;
    }
    void call(uint32_t nid, const Args &args, uint64_t sp = SP) {
        auto &p = cpu();
        p.gpr[1] = sp;
        for (unsigned i = 0; i < 8; ++i) p.gpr[3 + i] = args.r[i];
        const auto before = p;
        event_begin = event_count();
        CHECK(window.backing.read(window.backing.context, 0, before_memory, sizeof(before_memory)) == PS3_GUEST_MEMORY_OK);
        std::memset(window.rmask, 0, sizeof(window.rmask)); std::memset(window.wmask, 0, sizeof(window.wmask));
        window.reads = window.writes = 0;
        control->stop = StartupStop::None;
        active = control.get(); control->cpu = &p; terminal_guard = &guard;
        try { ps3_hle_call(nid, &p); active = nullptr; } catch (const Terminal &) {}
        stop = control->stop;
        CHECK(window.backing.read(window.backing.context, 0, after_memory, sizeof(after_memory)) == PS3_GUEST_MEMORY_OK);
        event_end = event_count();
        auto expected = before;
        if (stop == StartupStop::None) expected.gpr[3] = p.gpr[3];   // r3 checked by the caller; everything else preserved
        CHECK(!std::memcmp(&expected, &p, sizeof(p)));
        if (options.journal) CHECK(control->journal.count == event_end);
        else CHECK(!recorder.report().batch_active && !recorder.report().remaining_publications);
        // Instance-attribute, info and attach state is never touched by these routes.
        CHECK(control->spurs_attribute_calls == 0 && control->spurs_attribute_returns == 0 && control->spurs_attribute.nid == 0);
        CHECK(!control->spurs_attribute.write_attempted && !control->spurs_attribute.memory_checked && control->spurs_attribute.publications == 0);
    }
    bool changed_only(std::initializer_list<Range> allowed) {
        for (size_t i = 0; i < sizeof(before_memory); ++i) {
            const uint32_t address = GuestWindow::base + uint32_t(i); bool permitted = false;
            for (const auto &r : allowed) if (address >= r.address && uint64_t(address) - r.address < r.length) permitted = true;
            if (!permitted && before_memory[i] != after_memory[i]) return false;
        }
        return true;
    }
    static bool mask_is(const unsigned char *mask, std::initializer_list<Range> expected) {
        for (size_t i = 0; i < GuestWindow::span; ++i) {
            const uint32_t address = GuestWindow::base + uint32_t(i); bool want = false;
            for (const auto &r : expected) if (address >= r.address && uint64_t(address) - r.address < r.length) want = true;
            if (bool(mask[i]) != want) return false;
        }
        return true;
    }
    bool reads_are(std::initializer_list<Range> r) { return mask_is(window.rmask, r); }
    bool writes_are(std::initializer_list<Range> r) { return mask_is(window.wmask, r); }
    uint64_t mem(uint32_t address, size_t n) { return be(after_memory + (address - GuestWindow::base), n); }
    const unsigned char *at(uint32_t address) { return after_memory + (address - GuestWindow::base); }
    uint64_t stop_is(StartupStop s) { return stop == s; }
    // The common shape of a NATIVE STOP that happened before the named point: nothing written, no return state.
    bool no_return_no_write() {
        const auto &o = control->spurs_workload_attribute;
        return writes_are({}) && !o.toc_written && !o.has_result && !o.image_write_attempted && o.image_written_bytes == 0 &&
            control->spurs_workload_attribute_returns == 0;
    }
    bool name_no_return_no_write() {
        const auto &o = control->spurs_workload_name;
        return writes_are({}) && !o.toc_written && !o.has_result && !o.class_write_attempted && !o.instance_write_attempted &&
            o.written_bytes == 0 && control->spurs_workload_name_returns == 0;
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return g_tick; }
extern "C" [[noreturn]] void threadExit(void) {
    const int result = pthread_mutex_trylock(terminal_guard);
    CHECK(result == 0); if (!result) pthread_mutex_unlock(terminal_guard);
    throw Terminal{};
}

#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
namespace {
unsigned char kFull[512];
// Common expectations of an initializer that succeeded on the actual-like arguments.
void expect_actual_success(Runtime &r) {
    CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.cpu().gpr[2] == TOC);
    CHECK(!std::memcmp(r.at(ATTR), kFull, 512));
    CHECK(r.changed_only({{ATTR, 512}, {TOC_SLOT, 8}}));
    CHECK(r.reads_are({{STACK_SLOT, 8}, {PRIO, 8}}) && r.writes_are({{ATTR, 512}, {TOC_SLOT, 8}}));
    CHECK(r.mem(TOC_SLOT, 8) == TOC);
    CHECK(r.n_events() == 67 && r.ev(0, false, STACK_SLOT, 8, 5) && r.ev(1, false, PRIO, 8, UINT64_C(0x0001020f08000e07)));
    for (size_t i = 0; i < 64; ++i) CHECK(r.ev(2 + i, true, ATTR + 8 * uint32_t(i), 8, be(kFull + 8 * i, 8)));
    CHECK(r.ev(66, true, TOC_SLOT, 8, TOC));
}
}
#endif

int main() {
    std::printf("WORKLOAD_ATTR_LAYOUT startup_control=%zu attribute_observation=%zu name_observation=%zu\n",
        sizeof(StartupControl), sizeof(StartupSpursWorkloadAttributeObservation), sizeof(StartupSpursWorkloadNameObservation));
#ifndef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
    for (uint32_t nid : {0xefeb2679u, 0x4a5eab63u}) {
        Runtime r; r.seed_actual(); r.call(nid, Args());
        std::printf("WORKLOAD_ATTR_ROUTE feature=off nid=%08x stop=%d unknown_import=%d\n", nid, int(r.stop), int(r.stop == StartupStop::UnknownImport));
        CHECK(r.stop == StartupStop::UnknownImport && r.changed_only({}) && r.event_end == 0);
        CHECK(r.c().spurs_workload_attribute_calls == 0 && r.c().spurs_workload_name_calls == 0);
    }
#else
    std::memcpy(kFull, kHead, sizeof(kHead));
    // Route probe first: with the unchanged tree both NIDs are UnknownImport (stop=2), which is the red baseline.
    for (uint32_t nid : {INIT_NID, NAME_NID}) {
        Runtime r; r.seed_actual(); r.call(nid, Args());
        std::printf("WORKLOAD_ATTR_ROUTE feature=on nid=%08x stop=%d unknown_import=%d\n", nid, int(r.stop), int(r.stop == StartupStop::UnknownImport));
        CHECK(r.stop == StartupStop::None);
    }
    // ---- initializer: actual-like success, exact guest bytes, events, observation ----
    { Runtime r; r.seed_actual(); r.call(INIT_NID, Args()); expect_actual_success(r);
      const auto &o = r.c().spurs_workload_attribute;
      const uint64_t expected_args[8] = {ATTR, 1, 0x220002, PM, 0x2d80, UINT64_C(0x404e9300), PRIO, 1};
      for (int i = 0; i < 8; ++i) CHECK(o.arguments[i] == expected_args[i]);
      CHECK(o.sp == SP && o.stack_slot == STACK_SLOT && o.stack_raw == 5 && o.max_contention == 5 && o.priority_address == PRIO);
      CHECK(!std::memcmp(o.priority, kPriority, 8) && !std::memcmp(o.image, kFull, 512));
      CHECK(o.result == 0 && o.has_result && o.stack_read && o.priority_read && o.image_write_attempted && o.toc_written);
      CHECK(o.image_written_bytes == 512 && o.publications == 67 && o.memory_status == PS3_GUEST_MEMORY_OK);
      CHECK(r.c().spurs_workload_attribute_calls == 1 && r.c().spurs_workload_attribute_returns == 1);
      CHECK(r.c().spurs_workload_name_calls == 0 && r.c().import_count == 1 && r.c().imports[0] == INIT_NID);
      // A second call re-observes without disturbing the SetName observation.
      r.call(NAME_NID, Args());
      CHECK(r.c().spurs_workload_attribute_calls == 1 && r.c().spurs_workload_attribute.has_result && r.c().spurs_workload_name_calls == 1);
    }
    // ---- ninth argument: BE64 at (SP+0x70)&~3, low 32 bits (never the first BE32) ----
    for (const auto &t : {std::pair<uint64_t, uint32_t>{5, 5}, {UINT64_C(0x0000000500000000), 0}, {UINT64_C(0x0000000123456789), 0x23456789},
             {UINT64_C(0xffffffffffffffff), 0xffffffff}, {0, 0}}) {
        Runtime r; r.seed_actual(); r.seed_be64(STACK_SLOT, t.first); r.call(INIT_NID, Args());
        CHECK(r.stop == StartupStop::None && r.mem(ATTR + 0x24, 4) == t.second && r.mem(ATTR + 0x20, 4) == 1);
        CHECK(r.c().spurs_workload_attribute.stack_raw == t.first && r.c().spurs_workload_attribute.max_contention == t.second);
    }
    for (uint64_t sp : {UINT64_C(0xd00ff913), UINT64_C(0xd00ff912)}) {   // SP+0x70 is masked down to 4 bytes: 0xd00ff980
        Runtime r; r.seed_actual(); r.call(INIT_NID, Args(), sp);
        CHECK(r.stop == StartupStop::None && r.c().spurs_workload_attribute.stack_slot == STACK_SLOT && r.mem(ATTR + 0x24, 4) == 5);
        CHECK(r.ev(0, false, STACK_SLOT, 8, 5) && r.ev(66, true, uint32_t(sp) + 0x28, 8, TOC));
        CHECK(r.reads_are({{STACK_SLOT, 8}, {PRIO, 8}}));
    }
    // SP with high bits, wrap, an unmapped slot or an unwritable TOC slot STOP before every effect with no return state.
    for (uint64_t sp : {UINT64_C(0x1d00ff910), UINT64_C(0xffffff90), UINT64_C(0xfffffffffffffff0), UINT64_C(0xd0100fc0), UINT64_C(0xd0100fd8)}) {
        Runtime r; r.seed_actual(); r.call(INIT_NID, Args(), sp);
        CHECK(r.stop == StartupStop::InvalidMemory && r.event_end == 0 && r.no_return_no_write() && r.reads_are({}));
        CHECK(!r.c().spurs_workload_attribute.stack_read && !r.c().spurs_workload_attribute.priority_read && r.changed_only({}));
    }
    { Runtime r; r.seed_actual(); r.window.fail_read = STACK_SLOT; r.call(INIT_NID, Args());   // provider read fault of a permitted slot
      CHECK(r.stop == StartupStop::InvalidMemory && r.c().invalid_address == STACK_SLOT && r.no_return_no_write() && r.event_end == 0);
      CHECK(!r.c().spurs_workload_attribute.stack_read && r.c().spurs_workload_attribute.memory_status == PS3_GUEST_MEMORY_UNMAPPED); }
    { Options o; o.ram = 12288; Runtime r(o); r.call(INIT_NID, Args(), 0xd0100fc0);          // permitted but uncommitted slot: real domain refusal
      CHECK(r.stop == StartupStop::InvalidMemory && r.no_return_no_write() && !r.c().spurs_workload_attribute.stack_read);
      CHECK(r.c().spurs_workload_attribute.memory_status != PS3_GUEST_MEMORY_OK); }
    // The binder faults before the SDK body: an unmapped stack slot beats a NULL attribute.
    { Runtime r; Args a; a.r[0] = 0; r.call(INIT_NID, a, 0xd0100fc0); CHECK(r.stop == StartupStop::InvalidMemory && r.event_end == 0); }
    // ---- SDK precedence: attr NULL, attr%8, pm NULL, pm%16, all before the priority read and min == 0 ----
    struct Policy { uint64_t attr, pm, code; };
    for (const auto &t : {Policy{0, PM, SDK_NULL}, {0, 0, SDK_NULL}, {0, 8, SDK_NULL}, {1, 0, SDK_ALIGN}, {7, PM, SDK_ALIGN}, {ATTR + 4, 0, SDK_ALIGN},
             {ATTR, 0, SDK_NULL}, {ATTR, 8, SDK_ALIGN}, {ATTR, 0x69c88f, SDK_ALIGN}, {UINT64_C(0x100000000), PM, SDK_NULL},
             {UINT64_C(0x100000000) | ATTR, UINT64_C(0x200000000), SDK_NULL}}) {
        Runtime r; r.seed_actual(); Args a; a.r[0] = t.attr; a.r[3] = t.pm; a.r[6] = UNMAPPED; a.r[7] = 0;   // priority unmapped and min 0
        r.call(INIT_NID, a);
        const auto &o = r.c().spurs_workload_attribute;
        CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == t.code && o.has_result && o.result == uint32_t(t.code));
        CHECK(r.n_events() == 2 && r.ev(0, false, STACK_SLOT, 8, 5) && r.ev(1, true, TOC_SLOT, 8, TOC) && o.publications == 2);
        CHECK(r.reads_are({{STACK_SLOT, 8}}) && r.writes_are({{TOC_SLOT, 8}}) && r.changed_only({{TOC_SLOT, 8}}));
        CHECK(!o.priority_read && !o.image_write_attempted && o.image_written_bytes == 0 && o.toc_written && o.stack_read);
        CHECK(r.c().spurs_workload_attribute_returns == 1);
    }
    // ---- priority read: after pointer checks, before min == 0 ----
    { Runtime r; r.seed_actual(); Args a; a.r[7] = 0; r.call(INIT_NID, a);   // min 0 with readable priority -> INVAL, 3 events
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == SDK_INVAL && r.n_events() == 3 && r.ev(2, true, TOC_SLOT, 8, TOC));
      CHECK(r.reads_are({{STACK_SLOT, 8}, {PRIO, 8}}) && r.changed_only({{TOC_SLOT, 8}}) && r.c().spurs_workload_attribute.priority_read); }
    for (uint32_t prio : {UNMAPPED, 0u, 0xfffffffcu}) {    // min 0 + faulting priority: native STOP, never an invented SDK code
        Runtime r; r.seed_actual(); Args a; a.r[6] = prio; a.r[7] = 0; r.call(INIT_NID, a);
        const auto &o = r.c().spurs_workload_attribute;
        CHECK(r.stop == StartupStop::InvalidMemory && r.n_events() == 1 && r.ev(0, false, STACK_SLOT, 8, 5) && r.no_return_no_write());
        CHECK(o.stack_read && !o.priority_read && o.priority_address == prio && o.publications == 1 && r.c().invalid_address == prio);
    }
    { Runtime r; r.seed_actual(); r.window.fail_read = PRIO; r.call(INIT_NID, Args());
      CHECK(r.stop == StartupStop::InvalidMemory && r.n_events() == 1 && r.no_return_no_write() && !r.c().spurs_workload_attribute.priority_read);
      CHECK(r.c().spurs_workload_attribute.memory_status == PS3_GUEST_MEMORY_UNMAPPED); }
    { Options o; o.ram = 12288; Runtime r(o); r.seed_actual(); Args a; a.r[6] = UNMAPPED; r.call(INIT_NID, a);   // permitted, uncommitted priority
      CHECK(r.stop == StartupStop::InvalidMemory && r.n_events() == 1 && r.no_return_no_write() && r.c().spurs_workload_attribute.memory_status != PS3_GUEST_MEMORY_OK); }
    for (unsigned pos = 0; pos < 8; ++pos) for (unsigned char value : {0x10, 0xf0, 0x0f}) {
        Runtime r; r.seed_actual(); unsigned char prio[8]; std::memcpy(prio, kPriority, 8); prio[pos] = value; r.seed(PRIO, prio, 8);
        r.call(INIT_NID, Args());
        CHECK(r.stop == StartupStop::None);
        if (value == 0x0f) { CHECK(r.cpu().gpr[3] == 0 && !std::memcmp(r.at(ATTR + 0x18), prio, 8)); }
        else CHECK(r.cpu().gpr[3] == SDK_INVAL && r.n_events() == 3 && r.changed_only({{TOC_SLOT, 8}}));
    }
    { Runtime r; r.seed_actual(); r.seed_be64(STACK_SLOT, 3); Args a; a.r[1] = 0x3; a.r[2] = 0xffffffff; a.r[4] = 0x7fffffff; a.r[7] = 7;
      r.call(INIT_NID, a);   // odd revision, any sdk/size, min > max: no extra rejection
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.mem(ATTR, 4) == 3 && r.mem(ATTR + 4, 4) == 0xffffffff);
      CHECK(r.mem(ATTR + 0xc, 4) == 0x7fffffff && r.mem(ATTR + 0x20, 4) == 7 && r.mem(ATTR + 0x24, 4) == 3); }
    // A policy module that is aligned but unmapped is accepted and never dereferenced.
    for (uint32_t pm : {UNMAPPED, 0x10u, 0xfffffff0u}) {
        Runtime r; r.seed_actual(); Args a; a.r[3] = pm; r.call(INIT_NID, a);
        CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.mem(ATTR + 8, 4) == pm && r.reads_are({{STACK_SLOT, 8}, {PRIO, 8}}));
        CHECK(r.n_events() == 67);
    }
    // ---- aliasing: the 8 priority bytes are a snapshot taken before the 512-byte zero ----
    for (uint32_t prio : {ATTR + 0x18, ATTR + 0x100, TOC_SLOT, STACK_SLOT}) {
        Runtime r; r.seed_actual(); const unsigned char p[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        r.seed(prio, p, 8);                       // for STACK_SLOT this also replaces the ninth argument: max = 0x05060708
        Args a; a.r[6] = prio; r.call(INIT_NID, a);
        CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && !std::memcmp(r.at(ATTR + 0x18), p, 8));
        CHECK(r.ev(1, false, prio, 8, UINT64_C(0x0102030405060708)) && r.n_events() == 67);
        CHECK(r.mem(ATTR + 0x24, 4) == (prio == STACK_SLOT ? 0x05060708u : 5u));
        if (prio == ATTR + 0x100) CHECK(r.mem(ATTR + 0x100, 8) == 0);
        if (prio == TOC_SLOT) CHECK(r.mem(TOC_SLOT, 8) == TOC);
    }
    // ---- raw register narrowing: r3..r10 low 32 bits, data keeps its full 64 bits ----
    { Runtime r; r.seed_actual(); r.seed_be64(STACK_SLOT, UINT64_C(0x9999999900000007));
      Args a; a.r[0] = UINT64_C(0xabcdd00ff990); a.r[1] = UINT64_C(0x1111222233334444); a.r[2] = UINT64_C(0xaaaabbbbccccdddd);
      a.r[3] = UINT64_C(0x9999999900000000) | PM; a.r[4] = UINT64_C(0x5555555500002d80); a.r[5] = UINT64_C(0x0123456789abcdef);
      a.r[6] = UINT64_C(0x7777777700000000) | PRIO; a.r[7] = UINT64_C(0x100000003);
      r.call(INIT_NID, a);
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.mem(ATTR, 4) == 0x33334444 && r.mem(ATTR + 4, 4) == 0xccccdddd);
      CHECK(r.mem(ATTR + 8, 4) == PM && r.mem(ATTR + 0xc, 4) == 0x2d80 && r.mem(ATTR + 0x10, 8) == UINT64_C(0x0123456789abcdef));
      CHECK(!std::memcmp(r.at(ATTR + 0x18), kPriority, 8) && r.mem(ATTR + 0x20, 4) == 3 && r.mem(ATTR + 0x24, 4) == 7);
      CHECK(r.c().spurs_workload_attribute.arguments[0] == UINT64_C(0xabcdd00ff990) && r.c().spurs_workload_attribute.arguments[5] == UINT64_C(0x0123456789abcdef)); }
    { Runtime r; r.seed_actual(); Args a; a.r[7] = UINT64_C(0x100000000); r.call(INIT_NID, a);   // low 32 bits of min == 0 -> INVAL
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == SDK_INVAL); }
    // ---- native output preflight: nothing written, no TOC, no return ----
    for (uint32_t attr : {0xffffff00u, 0xd0100f00u, 0xd00ff800u, UNMAPPED}) {   // wrap, past RAM, overlaps the TOC slot, unmapped
        Runtime r; r.seed_actual(); Args a; a.r[0] = attr; r.call(INIT_NID, a);
        CHECK(r.stop == StartupStop::InvalidMemory && r.n_events() == 2 && r.no_return_no_write() && r.changed_only({}) && r.c().invalid_address == attr);
        CHECK(r.reads_are({{STACK_SLOT, 8}, {PRIO, 8}}) && r.c().spurs_workload_attribute.priority_read);
    }
    { Options o; o.readonly_attr = true; Runtime r(o); r.seed_actual(); r.call(INIT_NID, Args());
      CHECK(r.stop == StartupStop::InvalidMemory && r.n_events() == 2 && r.no_return_no_write() && r.changed_only({})); }
    { Runtime r; r.seed_actual(); Args a; a.r[0] = 0xd00ff940; r.call(INIT_NID, a);   // image overlaps the consumed stack slot: allowed
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.n_events() == 67 && r.mem(0xd00ff940 + 0x24, 4) == 5);
      CHECK(r.mem(STACK_SLOT, 8) == 0 && r.c().spurs_workload_attribute.stack_raw == 5 && r.mem(TOC_SLOT, 8) == TOC); }
    // ---- trace capacity: credits are checked before each effect; exact flags at every capacity ----
    for (int journal = 0; journal < 2; ++journal) for (size_t cap = 1; cap <= 67; ++cap) {
        Options o; o.trace = cap; o.journal = journal; Runtime r(o); r.seed_actual();
        if (journal) r.control->journal.capacity = cap;
        r.call(INIT_NID, Args());
        const auto &s = r.c().spurs_workload_attribute;
        if (cap >= 67) { CHECK(r.stop == StartupStop::None && s.has_result && s.publications == 67 && r.n_events() == 67); continue; }
        CHECK(r.stop == StartupStop::TraceCapacity && r.no_return_no_write());
        CHECK(s.stack_read == (cap >= 2) && s.priority_read == (cap >= 3) && r.n_events() == (cap >= 3 ? 2u : cap >= 2 ? 1u : 0u));
    }
    for (size_t cap : {size_t(1), size_t(2)}) {   // SDK pointer error needs read + TOC = 2
        Options o; o.trace = cap; Runtime r(o); r.seed_actual(); Args a; a.r[0] = 0; r.call(INIT_NID, a);
        CHECK(cap == 2 ? (r.stop == StartupStop::None && r.cpu().gpr[3] == SDK_NULL) : (r.stop == StartupStop::TraceCapacity && r.no_return_no_write()));
    }
    for (size_t cap : {size_t(2), size_t(3)}) {   // INVAL needs 2 reads + TOC = 3
        Options o; o.trace = cap; Runtime r(o); r.seed_actual(); Args a; a.r[7] = 0; r.call(INIT_NID, a);
        CHECK(cap == 3 ? (r.stop == StartupStop::None && r.cpu().gpr[3] == SDK_INVAL) : (r.stop == StartupStop::TraceCapacity && r.no_return_no_write() && r.n_events() == 1));
    }
    // ---- TOC written, publication failed: toc_written stays true, no return, no rollback ----
    { Options o; o.journal = true; Runtime r(o); r.seed_actual(); r.window.shrink_journal = TOC_SLOT; r.call(INIT_NID, Args());
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::TraceCapacity && s.toc_written && !s.has_result && s.image_written_bytes == 512 && s.publications == 66);
      CHECK(r.c().spurs_workload_attribute_returns == 0 && r.mem(TOC_SLOT, 8) == TOC && !std::memcmp(r.at(ATTR), kFull, 512) && r.n_events() == 66); }
    { Options o; o.journal = true; Runtime r(o); r.seed_actual(); r.window.shrink_journal = TOC_SLOT; Args a; a.r[0] = 0; r.call(INIT_NID, a);
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::TraceCapacity && s.toc_written && !s.has_result && s.publications == 1 && r.c().spurs_workload_attribute_returns == 0);
      CHECK(r.writes_are({{TOC_SLOT, 8}}) && r.mem(TOC_SLOT, 8) == TOC); }
    { Options o; o.journal = true; Runtime r(o); r.seed_actual(); r.window.shrink_journal = ATTR; r.call(INIT_NID, Args());   // image written, first publication fails
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::TraceCapacity && s.image_written_bytes == 512 && s.publications == 2 && !s.toc_written && !s.has_result);
      CHECK(!std::memcmp(r.at(ATTR), kFull, 512) && r.writes_are({{ATTR, 512}})); }
    // ---- cancellation / deadline at each boundary ----
    { Runtime r; r.seed_actual(); r.c().cancelled.store(true); r.call(INIT_NID, Args());
      CHECK(r.stop == StartupStop::Cancelled && r.c().spurs_workload_attribute_calls == 0 && r.event_end == 0 && r.changed_only({})); }
    { Runtime r; r.seed_actual(); r.window.cancel_read = STACK_SLOT; r.call(INIT_NID, Args());   // between the two reads
      CHECK(r.stop == StartupStop::Cancelled && r.n_events() == 1 && r.c().spurs_workload_attribute.stack_read && !r.c().spurs_workload_attribute.priority_read && r.no_return_no_write()); }
    { Runtime r; r.seed_actual(); r.window.cancel_read = PRIO; r.call(INIT_NID, Args());   // after the priority read, before the image
      CHECK(r.stop == StartupStop::Cancelled && r.n_events() == 2 && r.c().spurs_workload_attribute.priority_read && r.no_return_no_write()); }
    { Runtime r; r.seed_actual(); r.window.cancel_read = STACK_SLOT; Args a; a.r[0] = 0; r.call(INIT_NID, a);   // SDK error: before the TOC
      CHECK(r.stop == StartupStop::Cancelled && r.n_events() == 1 && r.no_return_no_write()); }
    { Runtime r; r.seed_actual(); r.window.cancel_write = ATTR; r.call(INIT_NID, Args());   // after the image: it stays, no TOC, no r3
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::Cancelled && s.image_written_bytes == 512 && s.publications == 66 && !s.toc_written && !s.has_result);
      CHECK(r.n_events() == 66 && !std::memcmp(r.at(ATTR), kFull, 512) && r.c().spurs_workload_attribute_returns == 0 && r.writes_are({{ATTR, 512}})); }
    { Runtime r; r.seed_actual(); r.c().deadline_tick = 50; r.window.tick_write = ATTR; r.call(INIT_NID, Args());
      CHECK(r.stop == StartupStop::Deadline && r.c().spurs_workload_attribute.image_written_bytes == 512 && !r.c().spurs_workload_attribute.toc_written); }
    { Runtime r; r.seed_actual(); r.window.cancel_write = TOC_SLOT; r.call(INIT_NID, Args());   // no poll after the TOC: the call completes
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.c().spurs_workload_attribute.has_result); }
    // ---- provider write failures ----
    { Runtime r; r.seed_actual(); r.window.fail_write = ATTR; r.call(INIT_NID, Args());
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::ProviderFailure && s.image_write_attempted && s.image_written_bytes == 0 && s.memory_status == PS3_GUEST_MEMORY_UNMAPPED);
      CHECK(r.n_events() == 2 && !s.toc_written && !s.has_result && r.writes_are({}) && r.changed_only({})); }
    { Runtime r; r.seed_actual(); r.window.partial_write = ATTR; r.call(INIT_NID, Args());   // bytes of a failed write are never claimed
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::ProviderFailure && s.image_write_attempted && s.image_written_bytes == 0 && !s.toc_written && !s.has_result);
      CHECK(r.n_events() == 2 && r.changed_only({{ATTR, 2}})); }
    { Runtime r; r.seed_actual(); r.window.fail_write = TOC_SLOT; r.call(INIT_NID, Args());
      const auto &s = r.c().spurs_workload_attribute;
      CHECK(r.stop == StartupStop::ProviderFailure && s.image_written_bytes == 512 && !s.toc_written && !s.has_result && s.publications == 66);
      CHECK(r.writes_are({{ATTR, 512}}) && r.c().spurs_workload_attribute_returns == 0); }
    // ---- coherence: a real PPU reservation is invalidated only by the bytes actually written ----
    for (uint32_t reserved : {0xd00ff990u, 0xd00ffa00u, 0xd00ffb80u, 0xd00ff900u}) {   // image lines and the TOC line
        Runtime r; uint32_t value = 0; int stored = -1; r.seed_actual();
        CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, reserved, &value) == PS3_GUEST_MEMORY_OK);
        r.call(INIT_NID, Args());
        CHECK(r.stop == StartupStop::None);
        CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, reserved, 123, &stored) == PS3_GUEST_MEMORY_OK && stored == 0);
    }
    for (uint32_t reserved : {PRIO, 0xd00ffc00u}) {   // read-only and untouched lines keep their reservation on success
        Runtime r; uint32_t value = 0; int stored = -1; r.seed_actual();
        CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, reserved, &value) == PS3_GUEST_MEMORY_OK);
        r.call(INIT_NID, Args());
        CHECK(r.stop == StartupStop::None);
        CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, reserved, 123, &stored) == PS3_GUEST_MEMORY_OK && stored == 1);
    }
    for (uint32_t reserved : {0xd00ffa00u, 0xd00ff900u}) {   // SDK-error branch writes only the TOC line
        Runtime r; uint32_t value = 0; int stored = -1; r.seed_actual(); Args a; a.r[0] = 0;
        CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, reserved, &value) == PS3_GUEST_MEMORY_OK);
        r.call(INIT_NID, a);
        CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, reserved, 123, &stored) == PS3_GUEST_MEMORY_OK && stored == (reserved == 0xd00ffa00u));
    }

    // ================= SetName =================
    unsigned char pattern[512]; for (size_t i = 0; i < 512; ++i) pattern[i] = static_cast<unsigned char>((i * 7 + 3) & 0xff);
    { Runtime r; r.seed(ATTR, pattern, 512); Args a; a.r[0] = ATTR; a.r[1] = 0xdeadbeef; a.r[2] = 0x01020304;
      r.call(NAME_NID, a);
      unsigned char expected[512]; std::memcpy(expected, pattern, 512);
      const unsigned char patch[8] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4}; std::memcpy(expected + 0x28, patch, 8);
      const auto &o = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && !std::memcmp(r.at(ATTR), expected, 512));
      CHECK(r.changed_only({{ATTR + 0x28, 8}, {TOC_SLOT, 8}}) && r.reads_are({}) && r.writes_are({{ATTR + 0x28, 8}, {TOC_SLOT, 8}}));
      CHECK(r.n_events() == 3 && r.ev(0, true, ATTR + 0x28, 4, 0xdeadbeef) && r.ev(1, true, ATTR + 0x2c, 4, 0x01020304) && r.ev(2, true, TOC_SLOT, 8, TOC));
      CHECK(r.mem(TOC_SLOT, 8) == TOC && o.arguments[0] == ATTR && o.arguments[1] == 0xdeadbeef && o.arguments[2] == 0x01020304);
      CHECK(o.attribute == ATTR && o.name_class == 0xdeadbeef && o.name_instance == 0x01020304 && o.patch_address == ATTR + 0x28 && !std::memcmp(o.patch, patch, 8));
      CHECK(o.class_write_attempted && o.class_written && o.instance_write_attempted && o.instance_written && o.toc_written && o.has_result && o.result == 0);
      CHECK(o.written_bytes == 8 && o.publications == 3 && o.memory_status == PS3_GUEST_MEMORY_OK);
      CHECK(r.c().spurs_workload_name_calls == 1 && r.c().spurs_workload_name_returns == 1 && r.c().spurs_workload_attribute_calls == 0 && !r.c().spurs_workload_attribute.has_result);
      CHECK(r.c().imports[0] == NAME_NID); }
    // Uninitialized memory is allowed; names are opaque numbers (NULL, equal, unmapped, inside the attribute, high bits).
    struct Names { uint64_t cls, inst; uint32_t expect_cls, expect_inst; };
    for (const auto &t : {Names{0, 0, 0, 0}, {0x1234, 0x1234, 0x1234, 0x1234}, {UNMAPPED, 0xfffffff0, UNMAPPED, 0xfffffff0},
             {ATTR + 0x28, ATTR + 0x2c, ATTR + 0x28, ATTR + 0x2c}, {UINT64_C(0x100000005), UINT64_C(0x2ffffffff), 5, 0xffffffff}, {ATTR, ATTR, ATTR, ATTR}}) {
        Runtime r; Args a; a.r[0] = ATTR; a.r[1] = t.cls; a.r[2] = t.inst; r.call(NAME_NID, a);
        CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == 0 && r.mem(ATTR + 0x28, 4) == t.expect_cls && r.mem(ATTR + 0x2c, 4) == t.expect_inst);
        CHECK(r.changed_only({{ATTR + 0x28, 8}, {TOC_SLOT, 8}}) && r.reads_are({}) && r.n_events() == 3);
        CHECK(r.ev(0, true, ATTR + 0x28, 4, t.expect_cls) && r.ev(1, true, ATTR + 0x2c, 4, t.expect_inst));
    }
    // Policy errors: NULL beats alignment; one TOC credit; the attribute and the names are untouched.
    for (const auto &t : {std::pair<uint64_t, uint64_t>{0, SDK_NULL}, {UINT64_C(0x100000000), SDK_NULL}, {1, SDK_ALIGN}, {7, SDK_ALIGN},
             {ATTR + 4, SDK_ALIGN}, {UINT64_C(0xabcd00000000) | (ATTR + 2), SDK_ALIGN}}) {
        Runtime r; r.seed(ATTR, pattern, 512); Args a; a.r[0] = t.first; a.r[1] = UNMAPPED; a.r[2] = 0; r.call(NAME_NID, a);
        const auto &o = r.c().spurs_workload_name;
        CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == t.second && o.has_result && o.result == uint32_t(t.second));
        CHECK(r.n_events() == 1 && r.ev(0, true, TOC_SLOT, 8, TOC) && o.publications == 1 && o.toc_written && !o.class_write_attempted && !o.instance_write_attempted);
        CHECK(r.writes_are({{TOC_SLOT, 8}}) && r.reads_are({}) && r.changed_only({{TOC_SLOT, 8}}) && o.written_bytes == 0 && r.c().spurs_workload_name_returns == 1);
    }
    // Native policy STOPs are distinct from SDK errors and write nothing.
    for (uint32_t attr : {0xfffffff8u, UNMAPPED, 0xd00ff910u, 0xd0100fd8u}) {   // wrap, unmapped, +0x28 is the TOC slot, +0x28 past RAM
        Runtime r; Args a; a.r[0] = attr; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
        CHECK(r.stop == StartupStop::InvalidMemory && r.event_end == 0 && r.name_no_return_no_write() && r.changed_only({}));
    }
    { Options o; o.readonly_attr = true; Runtime r(o); Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      CHECK(r.stop == StartupStop::InvalidMemory && r.event_end == 0 && r.name_no_return_no_write()); }
    { Runtime r; Args a; a.r[0] = 0xd0100fd0; a.r[1] = 0x11223344; a.r[2] = 0x55667788; r.call(NAME_NID, a);   // only 8 bytes are required, not 512
      CHECK(r.stop == StartupStop::None && r.mem(0xd0100ff8, 4) == 0x11223344 && r.mem(0xd0100ffc, 4) == 0x55667788 && r.changed_only({{0xd0100ff8, 8}, {TOC_SLOT, 8}})); }
    { Runtime r; Args a; r.call(NAME_NID, a, 0xd0100fd8); CHECK(r.stop == StartupStop::InvalidMemory && r.event_end == 0 && r.name_no_return_no_write()); }   // TOC slot
    for (int journal = 0; journal < 2; ++journal) for (size_t cap = 1; cap <= 3; ++cap) {
        Options o; o.trace = cap; o.journal = journal; Runtime r(o); if (journal) r.control->journal.capacity = cap;
        Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
        if (cap == 3) CHECK(r.stop == StartupStop::None && r.n_events() == 3);
        else CHECK(r.stop == StartupStop::TraceCapacity && r.name_no_return_no_write() && r.event_end == 0);
    }
    { Options o; o.trace = 1; Runtime r(o); Args a; a.r[0] = 0; r.call(NAME_NID, a); CHECK(r.stop == StartupStop::None && r.cpu().gpr[3] == SDK_NULL); }
    // Second-store failure preserves and reports the first; no rollback.
    { Runtime r; r.window.fail_write = ATTR + 0x2c; Args a; a.r[1] = 0xaaaaaaaa; a.r[2] = 0xbbbbbbbb; r.call(NAME_NID, a);
      const auto &o = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::ProviderFailure && o.class_write_attempted && o.class_written && o.instance_write_attempted && !o.instance_written);
      CHECK(o.written_bytes == 4 && o.publications == 1 && !o.toc_written && !o.has_result && o.memory_status == PS3_GUEST_MEMORY_UNMAPPED && r.c().spurs_workload_name_returns == 0);
      CHECK(r.mem(ATTR + 0x28, 4) == 0xaaaaaaaa && r.mem(ATTR + 0x2c, 4) == 0xa5a5a5a5 && r.n_events() == 1 && r.writes_are({{ATTR + 0x28, 4}})); }
    { Runtime r; r.window.fail_write = ATTR + 0x28; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &o = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::ProviderFailure && o.class_write_attempted && !o.class_written && !o.instance_write_attempted && o.written_bytes == 0 && r.event_end == 0 && r.changed_only({})); }
    { Runtime r; r.window.fail_write = TOC_SLOT; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &o = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::ProviderFailure && o.instance_written && !o.toc_written && !o.has_result && o.written_bytes == 8 && r.n_events() == 2); }
    // Cancellation / deadline between the stores and before the TOC; trace exhaustion after the first store and after the TOC.
    { Runtime r; r.window.cancel_write = ATTR + 0x28; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &o = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::Cancelled && o.class_written && !o.instance_write_attempted && o.publications == 1 && !o.toc_written && r.n_events() == 1 && !o.has_result); }
    { Runtime r; r.window.cancel_write = ATTR + 0x2c; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &o = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::Cancelled && o.instance_written && o.publications == 2 && !o.toc_written && r.n_events() == 2 && r.c().spurs_workload_name_returns == 0); }
    { Runtime r; r.c().deadline_tick = 50; r.window.tick_write = ATTR + 0x28; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      CHECK(r.stop == StartupStop::Deadline && r.c().spurs_workload_name.class_written && !r.c().spurs_workload_name.instance_write_attempted); }
    { Runtime r; r.c().cancelled.store(true); Args a; a.r[1] = 1; r.call(NAME_NID, a); CHECK(r.stop == StartupStop::Cancelled && r.c().spurs_workload_name_calls == 0 && r.changed_only({})); }
    { Options o; o.journal = true; Runtime r(o); r.window.shrink_journal = ATTR + 0x28; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &s = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::TraceCapacity && s.class_written && s.publications == 0 && !s.instance_write_attempted && !s.toc_written && r.mem(ATTR + 0x28, 4) == 1); }
    { Options o; o.journal = true; Runtime r(o); r.window.shrink_journal = ATTR + 0x2c; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &s = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::TraceCapacity && s.class_written && s.instance_written && s.publications == 1 && !s.toc_written && r.n_events() == 1); }
    { Options o; o.journal = true; Runtime r(o); r.window.shrink_journal = TOC_SLOT; Args a; a.r[1] = 1; a.r[2] = 2; r.call(NAME_NID, a);
      const auto &s = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::TraceCapacity && s.toc_written && !s.has_result && s.publications == 2 && r.c().spurs_workload_name_returns == 0 && r.mem(TOC_SLOT, 8) == TOC); }
    { Options o; o.journal = true; Runtime r(o); r.window.shrink_journal = TOC_SLOT; Args a; a.r[0] = 0; r.call(NAME_NID, a);
      const auto &s = r.c().spurs_workload_name;
      CHECK(r.stop == StartupStop::TraceCapacity && s.toc_written && !s.has_result && s.publications == 0 && r.c().spurs_workload_name_returns == 0); }
    // Coherence: reservations on the two stored words' line and the TOC line die, an untouched line survives.
    for (uint32_t reserved : {0xd00ff980u, 0xd00ff900u, 0xd00ffa00u}) {
        Runtime r; uint32_t value = 0; int stored = -1; Args a; a.r[1] = 1; a.r[2] = 2;
        CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, reserved, &value) == PS3_GUEST_MEMORY_OK);
        r.call(NAME_NID, a);
        CHECK(r.stop == StartupStop::None);
        CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, reserved, 123, &stored) == PS3_GUEST_MEMORY_OK && stored == (reserved == 0xd00ffa00u));
    }
    { Runtime r; uint32_t value = 0; int stored = -1; Args a; a.r[0] = 0;   // SDK error only touches the TOC line
      CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, 0xd00ff980u, &value) == PS3_GUEST_MEMORY_OK);
      r.call(NAME_NID, a);
      CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, 0xd00ff980u, 123, &stored) == PS3_GUEST_MEMORY_OK && stored == 1); }
    // The two observations are independent and survive each other.
    { Runtime r; r.seed_actual(); Args n; n.r[1] = 0x31; n.r[2] = 0x32; r.call(NAME_NID, n); r.call(INIT_NID, Args());
      CHECK(r.c().spurs_workload_name.has_result && r.c().spurs_workload_name.name_class == 0x31 && r.c().spurs_workload_attribute.has_result);
      CHECK(r.c().spurs_workload_name_calls == 1 && r.c().spurs_workload_attribute_calls == 1); }
#endif
    std::printf("WORKLOAD_ATTR_HOST checks=%d failures=%d native_runtime=false game_boot=false\n", checks, failures);
    return failures != 0;
}

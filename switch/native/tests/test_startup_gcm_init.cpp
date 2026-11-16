/* Focused host test for the native _cellGcmInitBody (NID 0x15BAE46B) route through the REAL
 * ps3_hle_call dispatcher, the owned StartupExecution, and the native GCM-init service plus RSX
 * front end. Bounded host memory only: no GPU, no game, no rendered frame, no Switch build. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "../adapters/guard_pthread.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("GCM_INIT_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};

constexpr uint32_t NID = 0x15bae46b;
constexpr uint64_t LR = 0x00446b18;
constexpr uint64_t TOC = 0x0078bf78;
// Host guest layout: the exact game request values are fixed by the service contract.
constexpr uint32_t GCM_WINDOW = 0x0e100000, GCM_WINDOW_SIZE = 0x20000;
constexpr uint32_t CTX_OUT = 0x009b56e8;
constexpr uint32_t IO_EA = 0x40700000, IO_WINDOW = 0x20000;
constexpr uint32_t STACK_BASE = 0xd00ff000, STACK_SPAN = 0x3000;
constexpr uint32_t STACK = STACK_BASE + 0x910, TOC_SLOT = STACK + 0x28;
constexpr uint32_t CONTEXT = GCM_WINDOW + 0x0000;
constexpr uint32_t OPD = GCM_WINDOW + 0x0100;
constexpr uint32_t CALLBACK_CODE = GCM_WINDOW + 0x0200;
constexpr uint32_t CONTROL = GCM_WINDOW + 0x1040;
constexpr uint32_t OFFSET_TABLE = GCM_WINDOW + 0x2000;
constexpr uint32_t IO_TABLE = GCM_WINDOW + 0x4000;
constexpr uint32_t REPORTS = GCM_WINDOW + 0x10000, REPORTS_SIZE = 0x10000;
constexpr uint32_t CONTEXT_END = IO_EA + 0x7ffc;
constexpr uint32_t CMD_SIZE = 0x00010000, IO_SIZE = 0x08500000;

/* Sparse host guest: one vector per region. commit/decommit succeed only inside a region. */
struct GcmGuest {
    struct Region { uint32_t ea; std::vector<unsigned char> bytes; };
    std::vector<Region> regions;
    Region &add(uint32_t ea, size_t size, unsigned char fill = 0) {
        regions.push_back(Region{ea, std::vector<unsigned char>(size, fill)});
        return regions.back();
    }
    Region *find(uint32_t ea, size_t length) {
        for (Region &r : regions)
            if (ea >= r.ea && uint64_t(ea) + length <= uint64_t(r.ea) + r.bytes.size()) return &r;
        return nullptr;
    }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) {
        auto &g = *static_cast<GcmGuest *>(p);
        return g.find(a, size_t(n)) ? PS3_GUEST_MEMORY_OK : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) {
        auto &g = *static_cast<GcmGuest *>(p);
        return g.find(a, size_t(n)) ? PS3_GUEST_MEMORY_OK : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n) {
        auto &g = *static_cast<GcmGuest *>(p);
        auto *r = g.find(a, size_t(n));
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memset(r->bytes.data() + (a - r->ea), 0, size_t(n));
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status read(void *p, uint32_t a, void *v, size_t n) {
        auto &g = *static_cast<GcmGuest *>(p);
        auto *r = g.find(a, n);
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memcpy(v, r->bytes.data() + (a - r->ea), n);
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *v, size_t n) {
        auto &g = *static_cast<GcmGuest *>(p);
        auto *r = g.find(a, n);
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memcpy(r->bytes.data() + (a - r->ea), v, n);
        return PS3_GUEST_MEMORY_OK;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};

enum class Omit { None, CtxOut, Image, Context, Opd, Control, Reports, Toc };

struct GcmRuntime {
    GcmGuest guest;
    ps3_pthread_guard guard{};
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[16]{};
    ps3_atomic_pin_record pins[128]{};
    std::vector<PpuMemoryRamRange> ranges;
    std::unique_ptr<PpuMemoryBinding> binding;
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(2048);
    PpuMemoryTraceRecorder recorder;
    Ps3PpuExecutionServices services{};
    gcm_init_service storage{};
    bool attached = false;
    bool use_service = true;

    explicit GcmRuntime(Omit omit = Omit::None) {
        guest.add(GCM_WINDOW, GCM_WINDOW_SIZE, 0xa5);
        guest.add(CTX_OUT, 4, 0xa5);
        guest.add(IO_EA, IO_WINDOW, 0xa5);
        guest.add(TOC - 0x69a0, 0x100, 0xa5);   // the caller/global window around TOC-0x69A0
        guest.add(STACK_BASE, STACK_SPAN, 0xa5);
        const struct { uint32_t ea; uint64_t size; Omit omit; } ram[] = {
            {CTX_OUT, 4, Omit::CtxOut},
            {IO_EA, IO_WINDOW, Omit::Image},
            {TOC - 0x69a0, 0x100, Omit::None},
        };
        ranges.push_back({GCM_WINDOW, 16, omit != Omit::Context});
        ranges.push_back({OPD, 8, omit != Omit::Opd});
        ranges.push_back({CONTROL, 12, omit != Omit::Control});
        ranges.push_back({REPORTS, REPORTS_SIZE, omit != Omit::Reports});
        for (const auto &r : ram)
            if (r.omit == Omit::None || r.omit != omit) ranges.push_back({r.ea, r.size, true});
        if (omit == Omit::Toc) {
            // Disjoint split so only the eight TOC bytes at SP+0x28 are unwritable.
            const uint64_t before = TOC_SLOT - STACK_BASE;
            ranges.push_back({STACK_BASE, before, true});
            ranges.push_back({TOC_SLOT + 8, STACK_SPAN - before - 8, true});
        } else {
            ranges.push_back({STACK_BASE, STACK_SPAN, true});
        }
        CHECK(!ps3_pthread_guard_init(&guard));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), ps3_pthread_guard_port(&guard),
            owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges.data(), ranges.size()));
        services.memory = &binding->coherent_memory;
        execution.control = control.get();
        execution.ppu.cpu.thread_id = 1;
        execution.ppu.cpu.gpr[1] = STACK;
        execution.ppu.cpu.gpr[2] = TOC;
        execution.ppu.cpu.lr = LR;
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Strict, 0));
        CHECK(native_startup_admit_recorded(execution, *binding, services, recorder));
        const unsigned char sentinel[8]{0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a};
        put(TOC_SLOT, sentinel, sizeof sentinel);
    }
    ~GcmRuntime() {
        if (execution.ppu.admitted) CHECK(ppu_memory_release_after_join(execution.ppu) == PS3_GUEST_MEMORY_OK);
        ps3_pthread_guard_destroy(&guard);
    }
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n) { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    uint64_t get(uint32_t a, size_t n) {
        unsigned char b[8]{}; CHECK(mem().read(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK);
        uint64_t v = 0; for (size_t i = 0; i < n; ++i) v = v << 8 | b[i]; return v;
    }
    void put32(uint32_t a, uint32_t v) { const unsigned char e[4]{uint8_t(v>>24),uint8_t(v>>16),uint8_t(v>>8),uint8_t(v)}; put(a,e,4); }
    std::vector<unsigned char> snapshot() {
        std::vector<unsigned char> out;
        for (const auto &r : guest.regions) out.insert(out.end(), r.bytes.begin(), r.bytes.end());
        return out;
    }
    bool attach() {
        const gcm_init_layout layout{CONTEXT, OPD, CALLBACK_CODE, TOC, CONTROL, REPORTS, REPORTS_SIZE,
            OFFSET_TABLE, IO_TABLE};
        attached = startup_gcm_init_attach(execution, use_service ? &storage : nullptr, layout) == GCM_INIT_OK;
        return attached;
    }
    void run(uint32_t nid, bool syscall) {
        auto &cpu = execution.ppu.cpu;
        control->stop = StartupStop::None;
        active = control.get();
        control->cpu = &cpu;
        try { (void)syscall; ps3_hle_call(nid, &cpu); active = nullptr; }
        catch (const Terminal &) { active = nullptr; }
    }
    void call(uint64_t ctx_out = CTX_OUT, uint64_t cmd = CMD_SIZE, uint64_t io_size = IO_SIZE,
              uint64_t io_ea = IO_EA, uint64_t lr = LR, uint64_t sp = STACK) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = sp; cpu.gpr[2] = TOC;
        cpu.gpr[3] = ctx_out; cpu.gpr[4] = cmd; cpu.gpr[5] = io_size; cpu.gpr[6] = io_ea;
        cpu.gpr[7] = cpu.gpr[8] = cpu.gpr[9] = UINT64_C(0xffffffffdead0000);
        cpu.lr = lr; cpu.cr = 0x12345678;
        run(NID, false);
    }
    size_t published() { return recorder.report().retained_count; }
    std::vector<PpuMemoryTraceEvent> since(size_t from) {
        std::vector<PpuMemoryTraceEvent> out; PpuMemoryTraceSample s;
        for (size_t i = from; recorder.read(i, s); ++i) out.push_back(s.event);
        return out;
    }
};

/* Mirrors the adapter's exact-count rule without calling it. */
size_t expected_events() {
    size_t events = 0; uint32_t i = 0;
    while (i < gcm_init_image_word_count) {
        const uint32_t start = gcm_init_image_words[i].io_offset; size_t used = 0;
        while (i < gcm_init_image_word_count && gcm_init_image_words[i].io_offset == start + uint32_t(used) &&
               used + 4u <= GCM_INIT_IMAGE_MAX_RUN_BYTES) { used += 4u; ++i; }
        events += (used == 1 || used == 2 || used == 4 || used == 8) ? 1
            : (used + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
    }
    return events + 1 + 1 + 3 + 5 + 1 + 1;
}

void check_success_bytes(GcmRuntime &r) {
    for (uint32_t i = 0; i < gcm_init_image_word_count; ++i)
        CHECK(r.get(IO_EA + gcm_init_image_words[i].io_offset, 4) == gcm_init_image_words[i].word);
    CHECK(r.get(CONTEXT + 0, 4) == IO_EA + GCM_INIT_IMAGE_START_GET);
    CHECK(r.get(CONTEXT + 4, 4) == CONTEXT_END);
    CHECK(r.get(CONTEXT + 8, 4) == IO_EA + GCM_INIT_IMAGE_PUT);
    CHECK(r.get(CONTEXT + 12, 4) == OPD);
    CHECK(uint32_t(r.get(CONTEXT + 8, 4)) <= uint32_t(r.get(CONTEXT + 4, 4)));
    CHECK(r.get(OPD + 0, 4) == CALLBACK_CODE && r.get(OPD + 4, 4) == TOC);
    CHECK(r.get(REPORTS + 0x00, 4) == 0 && r.get(REPORTS + 0x10, 4) == 0 && r.get(REPORTS + 0x20, 4) == 1);
    CHECK(r.get(CONTROL + 0, 4) == GCM_INIT_IMAGE_PUT);
    CHECK(r.get(CONTROL + 4, 4) == GCM_INIT_IMAGE_PUT);
    CHECK(r.get(CONTROL + 8, 4) == 0xFFFFFFFFu);
    CHECK(r.get(CTX_OUT, 4) == CONTEXT);
    CHECK(r.get(TOC_SLOT, 8) == TOC);
}

void success_and_exact_effect() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call();
    const auto &o = r->control->gcm_init;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.failure == StartupGcmInitFailure::None && o.has_result && o.toc_written);
    CHECK(o.raw_r3 == CTX_OUT && o.raw_r4 == CMD_SIZE && o.raw_r5 == IO_SIZE && o.raw_r6 == IO_EA);
    CHECK(o.request.ctx_out_ea == CTX_OUT && o.request.cmd_size == CMD_SIZE &&
          o.request.io_size == IO_SIZE && o.request.io_ea == IO_EA);
    CHECK(o.result.status == GCM_INIT_OK && o.result.r3 == 0);
    CHECK(o.result.methods_accepted == GCM_INIT_IMAGE_METHOD_COUNT);
    CHECK(o.result.references_published == 1 && o.result.final_ref == 0xFFFFFFFFu);
    CHECK(o.result.acquires_satisfied == 1);
    CHECK(r->control->gcm_init_calls == 1 && r->control->gcm_init_returns == 1);
    CHECK(o.publications == expected_events());
    CHECK(r->since(from).size() == expected_events());
    check_success_bytes(*r);
    // r7..r9 are ignored and preserved; CR is untouched.
    for (unsigned i = 7; i <= 9; ++i) CHECK(r->execution.ppu.cpu.gpr[i] == UINT64_C(0xffffffffdead0000));
    CHECK(r->execution.ppu.cpu.cr == 0x12345678);
    (void)before;
}

void pre_store_failures() {
    struct Case { const char *name; Omit omit; bool attach; uint64_t ctx, cmd, io_size, io_ea, lr; StartupGcmInitFailure failure; StartupStop stop; };
    const Case cases[] = {
        {"missing-service", Omit::None, false, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::MissingService, StartupStop::GcmInitStop},
        {"wrong-lr", Omit::None, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, 0x11111111, StartupGcmInitFailure::WrongLr, StartupStop::GcmInitStop},
        {"high-r3", Omit::None, true, UINT64_C(0x100000000)+CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::HighBits, StartupStop::GcmInitStop},
        {"high-r4", Omit::None, true, CTX_OUT, UINT64_C(0x100000000)+CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::HighBits, StartupStop::GcmInitStop},
        {"high-r5", Omit::None, true, CTX_OUT, CMD_SIZE, UINT64_C(0x100000000)+IO_SIZE, IO_EA, LR, StartupGcmInitFailure::HighBits, StartupStop::GcmInitStop},
        {"high-r6", Omit::None, true, CTX_OUT, CMD_SIZE, IO_SIZE, UINT64_C(0x100000000)+IO_EA, LR, StartupGcmInitFailure::HighBits, StartupStop::GcmInitStop},
        {"unwritable-ctx-out", Omit::CtxOut, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"unwritable-image", Omit::Image, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"unwritable-context", Omit::Context, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"unwritable-opd", Omit::Opd, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"unwritable-control", Omit::Control, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"unwritable-reports", Omit::Reports, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"unwritable-toc", Omit::Toc, true, CTX_OUT, CMD_SIZE, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"wrong-cmd-size", Omit::None, true, CTX_OUT, CMD_SIZE + 4, IO_SIZE, IO_EA, LR, StartupGcmInitFailure::ServiceRefused, StartupStop::GcmInitStop},
    };
    for (const Case &c : cases) {
        auto r = std::make_unique<GcmRuntime>(c.omit);
        r->use_service = c.attach;
        if (c.attach) CHECK(r->attach()); else CHECK(!r->attach());
        const auto before = r->snapshot();
        r->call(c.ctx, c.cmd, c.io_size, c.io_ea, c.lr);
        const auto &o = r->control->gcm_init;
        CHECK(r->control->stop == c.stop);
        CHECK(o.failure == c.failure);
        CHECK(!o.has_result && !r->control->gcm_init_returns);
        CHECK(r->snapshot() == before);
        CHECK(r->control->gcm_init_calls == 1);
    }
}

void second_call_is_refused() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    r->call();
    CHECK(r->control->stop == StartupStop::None && r->control->gcm_init_returns == 1);
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call();
    const auto &o = r->control->gcm_init;
    CHECK(r->control->stop == StartupStop::GcmInitStop);
    CHECK(o.failure == StartupGcmInitFailure::ServiceRefused);
    CHECK(o.result.status == GCM_INIT_ALREADY_INITIALIZED);
    CHECK(!o.has_result && !o.toc_written && o.publications == 0 && r->since(from).empty());
    CHECK(r->snapshot() == before);
    CHECK(r->control->gcm_init_calls == 2 && r->control->gcm_init_returns == 1);
}

void forced_flip_semaphore_fails() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    gcm_init_service_force_flip_semaphore(&r->storage, 0);
    r->call();
    const auto &o = r->control->gcm_init;
    CHECK(r->control->stop == StartupStop::GcmInitStop);
    CHECK(o.failure == StartupGcmInitFailure::ServiceFailed);
    CHECK(o.result.status == GCM_INIT_FRONTEND_FAILED || o.result.status == GCM_INIT_WALK_FAILED);
    CHECK(o.result.fe_status == GCM_FE_ACQUIRE_UNSATISFIED);
    CHECK(o.result.fe_method == 0x0068u);
    // The reference was never published and get never advanced.
    CHECK(r->get(CONTROL + 8, 4) == 0xa5a5a5a5u);
    CHECK(r->get(CONTROL + 4, 4) == GCM_INIT_IMAGE_START_GET);
    CHECK(!o.has_result && !r->control->gcm_init_returns);
}
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }

int main() {
    success_and_exact_effect();
    pre_store_failures();
    second_call_is_refused();
    forced_flip_semaphore_fails();
    std::printf("STARTUP_GCM_INIT_FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}

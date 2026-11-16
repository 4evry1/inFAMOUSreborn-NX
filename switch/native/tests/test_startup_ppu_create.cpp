/* Host behavior test for the generic sys_ppu_thread_create (sysPrxForUser NID 0x24a1ea07) bridge, StartupExecution::ppu_create.
 * Real collaborators: the actual startup_execution.cpp ps3_hle_call dispatcher and stop()/terminal-notification path, real shared
 * guest memory behind the atomic domain (tracked guards), the real PPU recorder/budget, and a real compiled-code lookup PORT. The
 * guest is a LABEL FIXTURE: a host thread seeds the PPU registers exactly as the import stub would and issues one real HLE call.
 * The StartupPpuCreatePort provider (Provider below) is a TEST FIXTURE that records what the bridge handed it and what guest memory
 * looked like at each callback; it is NOT the real child owner, creates no thread and runs no guest code. This is HOST test
 * evidence only: no game execution, no secondary PPU, no emulator/target. Expected values are derived by hand from the contract
 * (header comment of StartupPpuCreatePort), never captured from the implementation. */
#define main inherited_spurs_main
#include "test_startup_spurs_initialize.cpp"
#undef main
#include <cstring>
#include <string>

namespace {
std::atomic_int create_failures{0};
#define PCHECK(value) do { if (!(value)) { std::printf("PPU_CREATE_FAIL line=%d expression=%s\n", __LINE__, #value); \
    std::fflush(stdout); create_failures.fetch_add(1); } } while (0)

constexpr unsigned NID = 0x24a1ea07;
constexpr uint32_t OUT = 0x7D0208, OPD = 0x7D0300, NAME = 0x7D0400, UNMAPPED = 0xDEAD0000u, CHILD_ID = 0x0100002Bu;
constexpr uint32_t EXIT_OUT = 0x7D0210;   // the join's exit-value output (a distinct u64 in the same writable page)
constexpr uint64_t EXIT_SENTINEL = UINT64_C(0x0123456789ABCDEF), EXIT_VALUE = UINT64_C(0x00000000DEADBEEF);
constexpr uint32_t ENTRY = 0x00510A40, CHILD_TOC = 0x0098F3C0, ARG_ADDRESS = 0x7D0200;   // arbitrary fixture values, not game addresses
constexpr uint64_t OUT_SENTINEL = UINT64_C(0xCAFEF00DCAFEF00D), TOC_SENTINEL = UINT64_C(0x5555555555555555);
constexpr uint32_t TLS_SOURCE = 0x7CF100;
constexpr uint32_t CELL_FIXTURE = 0x80010004u;
const char NAME_TEXT[] = "Job Manager Event Handler";
void idle_trampoline(void *) {}
void dummy_function(ppu_context *) {}

struct Lookup {
    uint32_t known = ENTRY;
    unsigned calls = 0;
    uint32_t last = 0;
    ps3_compiled_code_lookup port{this, &Lookup::find};
    static ps3_compiled_code_status find(const void *context, uint32_t address, ps3_compiled_function *target) {
        Lookup &l = *const_cast<Lookup *>(static_cast<const Lookup *>(context));
        ++l.calls; l.last = address;
        if (address != l.known) return PS3_COMPILED_CODE_NOT_FOUND;
        *target = dummy_function;
        return PS3_COMPILED_CODE_OK;
    }
};

struct Case;
struct Obs {
    Case *c = nullptr;
    unsigned calls = 0;
    StartupStop stop = StartupStop::Returned;
    ppu_context terminal{};
    bool active_cleared = false, batch_closed = false;
};
void on_terminal(void *context, StartupControl *control);

// TEST FIXTURE provider: observes, never owns a real child.
struct Provider {
    Case *c = nullptr;
    unsigned prepare_calls = 0, start_calls = 0;
    StartupPpuCreateRequest request{};
    Ps3TlsTemplate tls{};
    const Ps3TlsTemplate *tls_pointer = nullptr;
    StartupPpuCreatePrepareResult prepare_reply{StartupPpuCreatePrepareStatus::Prepared, CHILD_ID, 0};
    StartupPpuCreateStartStatus start_reply = StartupPpuCreateStartStatus::Started;
    void (*prepare_hook)(Provider &) = nullptr, (*start_hook)(Provider &) = nullptr, (*join_hook)(Provider &) = nullptr;
    unsigned join_calls = 0;
    uint32_t join_id = 0;
    StartupPpuJoinResult join_reply{StartupPpuJoinStatus::ProviderFailure, 0};
    uint64_t out_at_join = 0;
    bool batch_idle_at_join = false, guards_free_at_join = false, active_ok_at_join = false;
    unsigned lookup_calls_at_prepare = 0;
    uint64_t out_at_prepare = 0, toc_at_prepare = 0, out_at_start = 0, toc_at_start = 0;
    uint32_t start_id = 0;
    bool batch_active_at_prepare = false, batch_idle_at_start = false, guards_free_at_start = false, active_ok = false;
    StartupPpuCreatePort port{this, &Provider::prepare, &Provider::start, &Provider::join};
    static StartupPpuCreatePrepareResult prepare(void *context, const StartupPpuCreateRequest *request, const Ps3TlsTemplate *tls);
    static StartupPpuCreateStartStatus start(void *context, uint32_t id);
    static StartupPpuJoinResult join(void *context, uint32_t id);
};

struct Options {
    bool lookup = true, lookup_find = true, tls_valid = true, port = true;
    size_t recorder_capacity = 0;   // nonzero: admit with a tiny strict recorder instead of the fixture's large one
};
struct Case {
    Lookup lookup;
    std::vector<PpuMemoryTraceEvent> small_events;
    PpuMemoryTraceRecorder small;
    std::unique_ptr<Runtime> rt{new Runtime(1, false, false)};
    Provider prov;
    Obs obs;
    ppu_context seeded{};
    bool returned = false;
    uint64_t published0 = 0;
    PpuMemoryTraceRecorder &recorder() { return small_events.empty() ? rt->recorder : small; }
    explicit Case(const Options &o = {}) {
        Runtime &r = *rt;
        if (o.lookup) {
            if (!o.lookup_find) lookup.port.find = nullptr;
            r.services.compiled_code = &lookup.port;
        }
        if (o.recorder_capacity) {
            small_events.resize(o.recorder_capacity);
            PCHECK(small.configure(small_events.data(), small_events.size(), PpuMemoryTraceRetention::Strict, 0));
            PCHECK(native_startup_admit_budgeted(r.execution, *r.binding, r.services, small, r.budget));
        } else {
            PCHECK(native_startup_admit_budgeted(r.execution, *r.binding, r.services, r.recorder, r.budget));
        }
        prov.c = this; obs.c = this;
        if (o.port) r.execution.ppu_create = &prov.port;
        if (o.tls_valid) { r.control->initialized_tls_template = {TLS_SOURCE, 4, 0x200}; r.control->initialized_tls_template_valid = true; }
        r.control->terminal_notification = {on_terminal, &obs};
        // Guest layout: OPD{BE entry, BE toc}, the name, a sentinel in the output u64 and in the saved-TOC stack slot.
        r.put32(OPD, ENTRY); r.put32(OPD + 4, CHILD_TOC);
        r.put(NAME, NAME_TEXT, sizeof NAME_TEXT);
        put64(OUT, OUT_SENTINEL); put64(TOC_SLOT, TOC_SENTINEL);
        put64(EXIT_OUT, EXIT_SENTINEL);
    }
    ps3_guest_memory mem() { return rt->mem(); }
    void put64(uint32_t a, uint64_t v) { unsigned char b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (56 - 8 * i)); rt->put(a, b, 8); }
    uint64_t get64(uint32_t a) { unsigned char b[8]{}; PCHECK(mem().read(mem().context, a, b, 8) == PS3_GUEST_MEMORY_OK);
        uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | b[i]; return v; }
    // The registers exactly as the generated import stub would set them for the real call shape.
    void seed() {
        auto &cpu = rt->execution.ppu.cpu;
        for (int i = 3; i < 32; ++i) if (i != 13) cpu.gpr[i] = UINT64_C(0x1111000000000000) + uint64_t(i);   // distinct garbage
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        cpu.gpr[3] = OUT; cpu.gpr[4] = OPD; cpu.gpr[5] = ARG_ADDRESS; cpu.gpr[6] = 249; cpu.gpr[7] = 0x10000; cpu.gpr[8] = 1; cpu.gpr[9] = NAME;
    }
    void call() {
        auto &cpu = rt->execution.ppu.cpu;
        seeded = cpu;
        published0 = recorder().report().published_count;
        rt->control->stop = StartupStop::None;
        active = rt->control.get(); rt->control->cpu = &cpu; g_trampoline_fn = idle_trampoline;
        try { ps3_hle_call(NID, &cpu); returned = true; active = nullptr; } catch (const Terminal &) { returned = false; }
        g_trampoline_fn = nullptr;
    }
    // The guest join call shape: r11 = 44, r3 = the id the bridge wrote, r4 = the exit-value output pointer.
    void seed_join(uint64_t id, uint64_t output) {
        auto &cpu = rt->execution.ppu.cpu;
        for (int i = 3; i < 32; ++i) if (i != 13) cpu.gpr[i] = UINT64_C(0x1111000000000000) + uint64_t(i);
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC; cpu.gpr[3] = id; cpu.gpr[4] = output; cpu.gpr[11] = 44;
    }
    void call_join() {
        auto &cpu = rt->execution.ppu.cpu;
        seeded = cpu;
        published0 = recorder().report().published_count;
        rt->control->stop = StartupStop::None;
        active = rt->control.get(); rt->control->cpu = &cpu; g_trampoline_fn = idle_trampoline;
        try { lv2_syscall(&cpu); returned = true; active = nullptr; } catch (const Terminal &) { returned = false; }
        g_trampoline_fn = nullptr;
    }
    static bool same(const ppu_context &a, const ppu_context &b) {
        for (int i = 0; i < 32; ++i) if (a.gpr[i] != b.gpr[i]) return false;
        return a.thread_id == b.thread_id && a.lr == b.lr;
    }
    void expect_stop(StartupStop expected, unsigned line) {
        if (returned || obs.calls != 1 || obs.stop != expected || rt->control->stop != expected || !obs.active_cleared ||
            !obs.batch_closed || active != nullptr || !same(obs.terminal, seeded) || !same(rt->execution.ppu.cpu, seeded)) {
            std::printf("PPU_CREATE_FAIL line=%u expected_stop=%d got_stop=%d returned=%d notify_calls=%u\n", line, int(expected), int(obs.stop), int(returned), obs.calls);
            create_failures.fetch_add(1);
        }
        PCHECK(!recorder().report().batch_active);
    }
    // Nothing the bridge was allowed to do before a provider effect happened.
    void expect_no_effects(unsigned line) {
        if (prov.prepare_calls || prov.start_calls || get64(OUT) != OUT_SENTINEL || get64(TOC_SLOT) != TOC_SENTINEL ||
            recorder().report().published_count != published0) {
            std::printf("PPU_CREATE_FAIL line=%u effects prepare=%u start=%u\n", line, prov.prepare_calls, prov.start_calls);
            create_failures.fetch_add(1);
        }
        const StartupControl &k = *rt->control;
        PCHECK(k.ppu_create_prepared == 0 && k.ppu_create_id_written == 0 && k.ppu_create_started == 0 && k.ppu_create_returns == 0);
    }
    void expect_snapshot_intact() {
        const StartupControl &k = *rt->control;
        if (k.initialized_tls_template_valid) PCHECK(k.initialized_tls_template.source == TLS_SOURCE && k.initialized_tls_template.file_size == 4 &&
            k.initialized_tls_template.memory_size == 0x200 && k.tls_returns == 0);
    }
};
void on_terminal(void *context, StartupControl *control) {
    Obs *o = static_cast<Obs *>(context);
    ++o->calls; o->stop = control->stop; o->terminal = control->terminal;
    o->active_cleared = active == nullptr;
    o->batch_closed = !o->c->recorder().report().batch_active;
}
StartupPpuCreatePrepareResult Provider::prepare(void *context, const StartupPpuCreateRequest *request, const Ps3TlsTemplate *tls) {
    Provider &p = *static_cast<Provider *>(context);
    ++p.prepare_calls;
    p.request = *request; p.tls = *tls; p.tls_pointer = tls;
    p.lookup_calls_at_prepare = p.c->lookup.calls;
    p.out_at_prepare = p.c->get64(OUT); p.toc_at_prepare = p.c->get64(TOC_SLOT);
    p.batch_active_at_prepare = p.c->recorder().report().batch_active;
    if (p.prepare_hook) p.prepare_hook(p);
    return p.prepare_reply;
}
StartupPpuCreateStartStatus Provider::start(void *context, uint32_t id) {
    Provider &p = *static_cast<Provider *>(context);
    ++p.start_calls; p.start_id = id;
    p.out_at_start = p.c->get64(OUT); p.toc_at_start = p.c->get64(TOC_SLOT);
    p.batch_idle_at_start = !p.c->recorder().report().batch_active;
    p.guards_free_at_start = thread_holds_no_tracked_guard();
    p.active_ok = active == p.c->rt->control.get();
    if (p.start_hook) p.start_hook(p);
    return p.start_reply;
}
StartupPpuJoinResult Provider::join(void *context, uint32_t id) {
    Provider &p = *static_cast<Provider *>(context);
    ++p.join_calls; p.join_id = id;
    p.out_at_join = p.c->get64(EXIT_OUT);
    p.batch_idle_at_join = !p.c->recorder().report().batch_active;
    p.guards_free_at_join = thread_holds_no_tracked_guard();
    p.active_ok_at_join = active == p.c->rt->control.get();
    if (p.join_hook) p.join_hook(p);
    return p.join_reply;
}

void case_missing_services() {
    struct Row { const char *name; Options o; void (*mutate)(Case &); };
    const Row rows[] = {
        {"no port", [] { Options o; o.port = false; return o; }(), nullptr},
        {"null prepare", {}, [](Case &c) { c.prov.port.prepare = nullptr; }},
        {"null start", {}, [](Case &c) { c.prov.port.start = nullptr; }},
        {"no compiled lookup", [] { Options o; o.lookup = false; return o; }(), nullptr},
        {"null find", [] { Options o; o.lookup_find = false; return o; }(), nullptr},
        {"no tls snapshot", [] { Options o; o.tls_valid = false; return o; }(), nullptr},
        {"no guest priority", {}, [](Case &c) { c.rt->execution.has_guest_priority = false; }},
        {"creator priority out of range", {}, [](Case &c) { c.rt->execution.guest_priority = 3072; }},
        {"creator priority negative", {}, [](Case &c) { c.rt->execution.guest_priority = -1; }}};
    for (const Row &row : rows) {
        Case c(row.o); if (row.mutate) row.mutate(c);
        c.seed(); c.call();
        if (c.obs.stop != StartupStop::MissingServices) std::printf("  row '%s' stop=%d\n", row.name, int(c.obs.stop));
        c.expect_stop(StartupStop::MissingServices, __LINE__);
        c.expect_no_effects(__LINE__);
        PCHECK(c.rt->control->ppu_create_calls == 1 && c.lookup.calls == 0);
        c.expect_snapshot_intact();
    }
}

void case_success_order() {
    Case c; c.seed();
    constexpr uint32_t private_region = 0x7D0500;
    c.prov.prepare_hook = [](Provider &p) {
        // Child TLS initialization uses this same coherent-memory port; it does not publish into the parent's recorder.
        const ps3_guest_memory &memory = p.c->rt->binding->coherent_memory;
        const unsigned char image[4] = {0x12, 0x34, 0x56, 0x78};
        PCHECK(memory.zero(memory.context, private_region, 16) == PS3_GUEST_MEMORY_OK);
        PCHECK(memory.write(memory.context, private_region, image, sizeof image) == PS3_GUEST_MEMORY_OK);
        PCHECK(p.c->recorder().report().batch_active);
        PCHECK(p.c->recorder().report().published_count == p.c->published0);
    };
    c.prov.start_hook = [](Provider &p) { PCHECK(p.c->get64(OUT) == CHILD_ID); };
    c.call();
    PCHECK(c.returned && c.obs.calls == 0 && c.rt->control->stop == StartupStop::None && active == nullptr);
    // Lookup happened BEFORE the provider allocated anything; prepare saw the decoded call and an UNTOUCHED guest.
    PCHECK(c.lookup.calls == 1 && c.lookup.last == ENTRY && c.prov.lookup_calls_at_prepare == 1);
    PCHECK(c.prov.prepare_calls == 1 && c.prov.start_calls == 1);
    const StartupPpuCreateRequest &q = c.prov.request;
    PCHECK(q.opd_address == OPD && q.entry == ENTRY && q.toc == CHILD_TOC && q.argument == ARG_ADDRESS && q.priority == 249 &&
           q.stack_size == 0x10000 && q.flags == 1 && q.output == OUT && q.name_address == NAME &&
           q.name_length == sizeof NAME_TEXT - 1 && std::strcmp(q.name, NAME_TEXT) == 0 && q.creator_thread_id == 1 && q.creator_priority == 250);
    PCHECK(c.prov.tls.source == TLS_SOURCE && c.prov.tls.file_size == 4 && c.prov.tls.memory_size == 0x200);
    PCHECK(c.prov.tls_pointer != nullptr && c.prov.tls_pointer != &c.rt->control->initialized_tls_template);   // a copy, not the live snapshot
    PCHECK(c.prov.out_at_prepare == OUT_SENTINEL && c.prov.toc_at_prepare == TOC_SENTINEL);   // prepare never writes the id
    PCHECK(c.prov.batch_active_at_prepare);
    unsigned char private_bytes[16]{};
    PCHECK(c.mem().read(c.mem().context, private_region, private_bytes, sizeof private_bytes) == PS3_GUEST_MEMORY_OK);
    const unsigned char expected_private[16] = {0x12, 0x34, 0x56, 0x78};
    PCHECK(std::memcmp(private_bytes, expected_private, sizeof private_bytes) == 0);
    // The id (BE u64) and the saved TOC were visible BEFORE start, with nothing held across it.
    PCHECK(c.prov.start_id == CHILD_ID && c.prov.out_at_start == CHILD_ID && c.prov.toc_at_start == TOC);
    PCHECK(c.prov.batch_idle_at_start && c.prov.guards_free_at_start && c.prov.active_ok);
    unsigned char raw[8]{}; PCHECK(c.mem().read(c.mem().context, OUT, raw, 8) == PS3_GUEST_MEMORY_OK);
    PCHECK(raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0 && raw[4] == 0x01 && raw[5] == 0x00 && raw[6] == 0x00 && raw[7] == 0x2B);
    // Return: r3 = 0 and r2 preserved; nothing else changes.
    auto &cpu = c.rt->execution.ppu.cpu;
    PCHECK(cpu.gpr[3] == 0 && cpu.gpr[2] == TOC);
    ppu_context expect = c.seeded; expect.gpr[3] = 0;
    PCHECK(Case::same(cpu, expect));
    // Counters and trace: exactly the id write then the TOC publication.
    const StartupControl &k = *c.rt->control;
    PCHECK(k.ppu_create_calls == 1 && k.ppu_create_prepared == 1 && k.ppu_create_id_written == 1 && k.ppu_create_started == 1 &&
           k.ppu_create_returns == 1 && k.ppu_create_guest_errors == 0 && k.ppu_create_terminal == 0);
    PCHECK(k.ppu_create_last_entry == ENTRY && k.ppu_create_last_toc == CHILD_TOC && k.ppu_create_last_output == OUT &&
           k.ppu_create_last_thread_id == CHILD_ID && k.ppu_create_last_priority == 249 && k.ppu_create_last_stack == 0x10000 &&
           k.ppu_create_last_argument == ARG_ADDRESS && k.ppu_create_last_flags == 1);
    PCHECK(k.ppu_create_has_prepare && k.ppu_create_prepare == StartupPpuCreatePrepareStatus::Prepared &&
           k.ppu_create_has_start && k.ppu_create_start == StartupPpuCreateStartStatus::Started);
    PCHECK(c.recorder().report().published_count == c.published0 + 2 && !c.recorder().report().batch_active);
    const auto &e0 = c.rt->events[size_t(c.published0)], &e1 = c.rt->events[size_t(c.published0) + 1];
    PCHECK(e0.address == OUT && e0.length == 8 && e1.address == TOC_SLOT && e1.length == 8);
    PCHECK(c.get64(TOC_SLOT) == TOC);
    c.expect_snapshot_intact();
}
void case_success_boundaries() {
    {   // 127-character name (NUL is byte 127 of the 128-byte scan) and a full-width 64-bit argument.
        Case c; c.seed();
        std::string name(127, 'n'); c.rt->put(NAME, name.c_str(), name.size() + 1);
        c.rt->execution.ppu.cpu.gpr[5] = UINT64_C(0xFFFF0000007D0200);
        c.call();
        PCHECK(c.returned && c.prov.prepare_calls == 1 && c.prov.start_calls == 1 && c.prov.request.name_length == 127 &&
               std::strcmp(c.prov.request.name, name.c_str()) == 0 && c.prov.request.argument == UINT64_C(0xFFFF0000007D0200));
    }
    {   // The output u64 may overlap the OPD: the OPD is decoded before the id write, so this is safe and not over-restricted.
        Case c; c.seed();
        c.rt->execution.ppu.cpu.gpr[3] = OPD;
        c.call();
        PCHECK(c.returned && c.prov.prepare_calls == 1 && c.prov.request.entry == ENTRY && c.prov.request.toc == CHILD_TOC);
        PCHECK(c.get64(OPD) == CHILD_ID && c.get64(OUT) == OUT_SENTINEL);   // the id landed at the overlapped OPD, not at OUT
    }
}

void case_validation() {
    struct Row { const char *name; void (*mutate)(Case &); StartupStop stop; unsigned lookup_calls; };
    const Row rows[] = {
        {"output null", [](Case &c) { c.rt->execution.ppu.cpu.gpr[3] = 0; }, StartupStop::InvalidMemory, 0},
        {"output high bits", [](Case &c) { c.rt->execution.ppu.cpu.gpr[3] = (UINT64_C(1) << 32) | OUT; }, StartupStop::InvalidMemory, 0},
        {"output misaligned", [](Case &c) { c.rt->execution.ppu.cpu.gpr[3] = OUT + 2; }, StartupStop::InvalidMemory, 0},   // 4-aligned is valid (the game does it)
        {"output unmapped", [](Case &c) { c.rt->execution.ppu.cpu.gpr[3] = UNMAPPED; }, StartupStop::InvalidMemory, 0},
        {"output at range end", [](Case &c) { c.rt->execution.ppu.cpu.gpr[3] = 0x7D2000; }, StartupStop::InvalidMemory, 0},
        {"toc slot unmapped", [](Case &c) { c.rt->execution.ppu.cpu.gpr[1] = UNMAPPED; }, StartupStop::InvalidMemory, 0},
        {"toc slot overflow", [](Case &c) { c.rt->execution.ppu.cpu.gpr[1] = UINT64_MAX - 0x10; }, StartupStop::InvalidMemory, 0},
        {"output overlaps toc slot", [](Case &c) { c.rt->execution.ppu.cpu.gpr[3] = TOC_SLOT; c.rt->execution.ppu.cpu.gpr[1] = STACK; }, StartupStop::InvalidMemory, 0},
        {"flags 0", [](Case &c) { c.rt->execution.ppu.cpu.gpr[8] = 0; }, StartupStop::UnsupportedSyscall, 0},
        {"flags 2", [](Case &c) { c.rt->execution.ppu.cpu.gpr[8] = 2; }, StartupStop::UnsupportedSyscall, 0},
        {"flags high bit", [](Case &c) { c.rt->execution.ppu.cpu.gpr[8] = (UINT64_C(1) << 32) | 1; }, StartupStop::UnsupportedSyscall, 0},
        {"priority 3072", [](Case &c) { c.rt->execution.ppu.cpu.gpr[6] = 3072; }, StartupStop::UnsupportedSyscall, 0},
        {"priority negative", [](Case &c) { c.rt->execution.ppu.cpu.gpr[6] = UINT64_C(0xFFFFFFFFFFFFFFFF); }, StartupStop::UnsupportedSyscall, 0},
        {"stack zero", [](Case &c) { c.rt->execution.ppu.cpu.gpr[7] = 0; }, StartupStop::UnsupportedSyscall, 0},
        {"opd null", [](Case &c) { c.rt->execution.ppu.cpu.gpr[4] = 0; }, StartupStop::InvalidMemory, 0},
        {"opd high bits", [](Case &c) { c.rt->execution.ppu.cpu.gpr[4] = (UINT64_C(1) << 32) | OPD; }, StartupStop::InvalidMemory, 0},
        {"opd misaligned", [](Case &c) { c.rt->execution.ppu.cpu.gpr[4] = OPD + 2; }, StartupStop::InvalidMemory, 0},
        {"opd unmapped", [](Case &c) { c.rt->execution.ppu.cpu.gpr[4] = UNMAPPED; }, StartupStop::InvalidMemory, 0},
        {"opd straddles end", [](Case &c) { c.rt->execution.ppu.cpu.gpr[4] = 0x7D2000 - 4; }, StartupStop::InvalidMemory, 0},
        {"entry zero", [](Case &c) { c.put64(OPD, 0); }, StartupStop::DispatchFailure, 0},
        {"entry misaligned", [](Case &c) { c.rt->put32(OPD, ENTRY + 2); }, StartupStop::DispatchFailure, 0},
        {"entry not compiled", [](Case &c) { c.rt->put32(OPD, ENTRY + 4); }, StartupStop::DispatchFailure, 1},
        {"name null", [](Case &c) { c.rt->execution.ppu.cpu.gpr[9] = 0; }, StartupStop::UnsupportedSyscall, 1},
        {"name unmapped", [](Case &c) { c.rt->execution.ppu.cpu.gpr[9] = UNMAPPED; }, StartupStop::InvalidMemory, 1},
        {"name high bits", [](Case &c) { c.rt->execution.ppu.cpu.gpr[9] = (UINT64_C(1) << 32) | NAME; }, StartupStop::InvalidMemory, 1},
        {"name 128 bytes no NUL", [](Case &c) { std::string n(128, 'x'); c.rt->put(NAME, n.data(), n.size()); c.rt->put(NAME + 128, "y", 1); }, StartupStop::UnsupportedSyscall, 1},
        {"name runs off mapped end", [](Case &c) { std::string n(16, 'x'); c.rt->put(0x7D2000 - 16, n.data(), n.size()); c.rt->execution.ppu.cpu.gpr[9] = 0x7D2000 - 16; }, StartupStop::InvalidMemory, 1}};
    for (const Row &row : rows) {
        Case c; c.seed(); row.mutate(c);
        c.call();
        if (c.obs.stop != row.stop || c.lookup.calls != row.lookup_calls) std::printf("  row '%s' stop=%d lookup_calls=%u\n", row.name, int(c.obs.stop), c.lookup.calls);
        c.expect_stop(row.stop, __LINE__);
        c.expect_no_effects(__LINE__);
        PCHECK(c.lookup.calls == row.lookup_calls && c.rt->control->ppu_create_calls == 1);
        c.expect_snapshot_intact();
    }
}
void case_trace_exhaustion() {
    Options o; o.recorder_capacity = 1;   // the call needs two published records (id + TOC)
    Case c(o); c.seed(); c.call();
    c.expect_stop(StartupStop::TraceCapacity, __LINE__);
    c.expect_no_effects(__LINE__);
    PCHECK(c.lookup.calls == 1 && c.rt->control->ppu_create_calls == 1);
    // Exactly two records is enough: same bridge, same fixture, now it succeeds.
    Options two; two.recorder_capacity = 2;
    Case d(two); d.seed(); d.call();
    PCHECK(d.returned && d.prov.start_calls == 1 && d.recorder().report().published_count == 2);
}

void case_prepare_outcomes() {
    struct Row { const char *name; StartupPpuCreatePrepareStatus status; uint32_t id; uint32_t cell; StartupStop stop; };
    const Row rows[] = {
        {"unsupported", StartupPpuCreatePrepareStatus::Unsupported, 0, 0, StartupStop::UnsupportedSyscall},
        {"provider failure", StartupPpuCreatePrepareStatus::ProviderFailure, 0, 0, StartupStop::ProviderFailure},
        {"out of range status", StartupPpuCreatePrepareStatus(99), CHILD_ID, 0, StartupStop::ProviderFailure},
        {"prepared id zero", StartupPpuCreatePrepareStatus::Prepared, 0, 0, StartupStop::ProviderFailure},
        {"guest error with cell zero", StartupPpuCreatePrepareStatus::GuestError, 0, 0, StartupStop::ProviderFailure}};
    for (const Row &row : rows) {
        Case c; c.seed();
        c.prov.prepare_reply = {row.status, row.id, row.cell};
        c.call();
        if (c.obs.stop != row.stop) std::printf("  row '%s' stop=%d\n", row.name, int(c.obs.stop));
        c.expect_stop(row.stop, __LINE__);
        PCHECK(c.prov.prepare_calls == 1 && c.prov.start_calls == 0 && c.get64(OUT) == OUT_SENTINEL && c.get64(TOC_SLOT) == TOC_SENTINEL);
        PCHECK(c.recorder().report().published_count == c.published0);   // no id invented, no TOC published on a stop
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_create_prepared == 0 && k.ppu_create_id_written == 0 && k.ppu_create_started == 0 && k.ppu_create_returns == 0);
        c.expect_snapshot_intact();
    }
    {   // A completed guest error: no child, no id, no start; r3 carries the sign-extended error and r2 is preserved.
        Case c; c.seed();
        c.prov.prepare_reply = {StartupPpuCreatePrepareStatus::GuestError, 0, CELL_FIXTURE};
        c.call();
        PCHECK(c.returned && c.obs.calls == 0 && c.rt->control->stop == StartupStop::None);
        PCHECK(c.prov.prepare_calls == 1 && c.prov.start_calls == 0 && c.get64(OUT) == OUT_SENTINEL);
        auto &cpu = c.rt->execution.ppu.cpu;
        PCHECK(cpu.gpr[3] == UINT64_C(0xFFFFFFFF80010004) && cpu.gpr[2] == TOC);
        ppu_context expect = c.seeded; expect.gpr[3] = UINT64_C(0xFFFFFFFF80010004);
        PCHECK(Case::same(cpu, expect));
        PCHECK(c.get64(TOC_SLOT) == TOC);   // the import stub's TOC save is still performed
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_create_calls == 1 && k.ppu_create_prepared == 0 && k.ppu_create_guest_errors == 1 && k.ppu_create_id_written == 0 &&
               k.ppu_create_started == 0 && k.ppu_create_returns == 1 && k.ppu_create_last_cell == CELL_FIXTURE);
        PCHECK(k.ppu_create_prepare == StartupPpuCreatePrepareStatus::GuestError && !k.ppu_create_has_start);
        PCHECK(!c.recorder().report().batch_active && c.recorder().report().published_count == c.published0 + 1);
    }
}
void case_cancellation() {
    {   // Cancel observed while prepare was running: the prepared child stays provider-owned; no id, no start, no r3.
        Case c; c.seed();
        c.prov.prepare_hook = [](Provider &p) { p.c->rt->control->cancelled.store(true); };
        c.call();
        c.expect_stop(StartupStop::Cancelled, __LINE__);
        PCHECK(c.prov.prepare_calls == 1 && c.prov.start_calls == 0 && c.get64(OUT) == OUT_SENTINEL && c.get64(TOC_SLOT) == TOC_SENTINEL);
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_create_prepared == 1 && k.ppu_create_last_thread_id == CHILD_ID && k.ppu_create_id_written == 0 && k.ppu_create_started == 0 && k.ppu_create_returns == 0);
        PCHECK(c.recorder().report().published_count == c.published0);
    }
    {   // Cancel observed while start ran (child really started): the effects stay committed, the parent gets no r3.
        Case c; c.seed();
        c.prov.start_hook = [](Provider &p) { p.c->rt->control->cancelled.store(true); };
        c.call();
        c.expect_stop(StartupStop::Cancelled, __LINE__);
        PCHECK(c.prov.prepare_calls == 1 && c.prov.start_calls == 1 && c.get64(OUT) == CHILD_ID && c.get64(TOC_SLOT) == TOC);
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_create_id_written == 1 && k.ppu_create_started == 1 && k.ppu_create_returns == 0 && k.ppu_create_start == StartupPpuCreateStartStatus::Started);
    }
    {   // Already cancelled on entry: the generic boundary stops before any validation, lookup or provider call.
        Case c; c.seed();
        c.rt->control->cancelled.store(true);
        c.call();
        c.expect_stop(StartupStop::Cancelled, __LINE__);
        c.expect_no_effects(__LINE__);
        PCHECK(c.lookup.calls == 0);
    }
}
void case_start_failures() {
    struct Row { const char *name; StartupPpuCreateStartStatus status; StartupStop stop; size_t terminal; };
    const Row rows[] = {
        {"terminal", StartupPpuCreateStartStatus::Terminal, StartupStop::ProviderFailure, 1},
        {"unsupported", StartupPpuCreateStartStatus::Unsupported, StartupStop::UnsupportedSyscall, 0},
        {"provider failure", StartupPpuCreateStartStatus::ProviderFailure, StartupStop::ProviderFailure, 0},
        {"out of range status", StartupPpuCreateStartStatus(99), StartupStop::ProviderFailure, 0}};
    for (const Row &row : rows) {
        Case c; c.seed();
        c.prov.start_reply = row.status;
        c.call();
        if (c.obs.stop != row.stop) std::printf("  row '%s' stop=%d\n", row.name, int(c.obs.stop));
        c.expect_stop(row.stop, __LINE__);
        // The id and TOC were committed before start and stay committed; the child stays provider-owned (nothing was undone).
        PCHECK(c.prov.prepare_calls == 1 && c.prov.start_calls == 1 && c.get64(OUT) == CHILD_ID && c.get64(TOC_SLOT) == TOC);
        PCHECK(c.prov.batch_idle_at_start && c.prov.guards_free_at_start);
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_create_prepared == 1 && k.ppu_create_id_written == 1 && k.ppu_create_started == 0 && k.ppu_create_returns == 0 &&
               k.ppu_create_terminal == row.terminal && k.ppu_create_has_start);
        PCHECK(c.recorder().report().published_count == c.published0 + 2);
    }
}
void case_join_missing_services() {
    struct Row { const char *name; bool port; void (*mutate)(Case &); };
    const Row rows[] = {
        {"no port", false, nullptr},
        {"null join", true, [](Case &c) { c.prov.port.join = nullptr; }}};
    for (const Row &row : rows) {
        Case c; if (!row.port) c.rt->execution.ppu_create = nullptr;
        if (row.mutate) row.mutate(c);
        c.seed_join(CHILD_ID, EXIT_OUT); c.call_join();
        if (c.obs.stop != StartupStop::MissingServices) std::printf("  join row '%s' stop=%d\n", row.name, int(c.obs.stop));
        c.expect_stop(StartupStop::MissingServices, __LINE__);
        PCHECK(c.prov.join_calls == 0 && c.get64(EXIT_OUT) == EXIT_SENTINEL);
        PCHECK(c.recorder().report().published_count == c.published0);
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_join_calls == 1 && k.ppu_join_returns == 0 && !k.ppu_join_has_result);
    }
}

void case_join_success() {
    Case c; c.seed_join(CHILD_ID, EXIT_OUT);
    c.prov.join_reply = {StartupPpuJoinStatus::Joined, EXIT_VALUE};
    c.prov.join_hook = [](Provider &p) { PCHECK(p.c->get64(EXIT_OUT) == EXIT_SENTINEL); };   // nothing written before the provider answered
    c.call_join();
    PCHECK(c.returned && c.obs.calls == 0 && c.rt->control->stop == StartupStop::None && active == nullptr);
    PCHECK(c.prov.join_calls == 1 && c.prov.join_id == CHILD_ID);
    // The exit value is a BE u64 at the guest output pointer.
    unsigned char raw[8]{}; PCHECK(c.mem().read(c.mem().context, EXIT_OUT, raw, 8) == PS3_GUEST_MEMORY_OK);
    PCHECK(raw[0] == 0 && raw[1] == 0 && raw[2] == 0 && raw[3] == 0 && raw[4] == 0xDE && raw[5] == 0xAD && raw[6] == 0xBE && raw[7] == 0xEF);
    PCHECK(c.get64(EXIT_OUT) == EXIT_VALUE);
    // No reservation, pin or guard is held while the provider runs.
    PCHECK(c.prov.batch_idle_at_join && c.prov.guards_free_at_join && c.prov.active_ok_at_join);
    // Return: r3 = 0, everything else unchanged.
    auto &cpu = c.rt->execution.ppu.cpu;
    PCHECK(cpu.gpr[3] == 0);
    ppu_context expect = c.seeded; expect.gpr[3] = 0;
    PCHECK(Case::same(cpu, expect));
    // Counters and the single trace record.
    const StartupControl &k = *c.rt->control;
    PCHECK(k.ppu_join_calls == 1 && k.ppu_join_returns == 1 && k.ppu_join_has_result &&
           k.ppu_join_last_status == StartupPpuJoinStatus::Joined && k.ppu_join_last_thread == CHILD_ID &&
           k.ppu_join_last_exit_value == EXIT_VALUE);
    PCHECK(c.recorder().report().published_count == c.published0 + 1 && !c.recorder().report().batch_active);
    PCHECK(c.rt->events[size_t(c.published0)].address == EXIT_OUT && c.rt->events[size_t(c.published0)].length == 8);
    // The create counters stay untouched: this was a join, not a create.
    PCHECK(k.ppu_create_calls == 0 && k.ppu_create_returns == 0);
    // A full-width id above the 32-bit id space is still a valid id once its high word is zero; a nonzero high word is a stop.
    {   // zero exit value is a legitimate exit value, not a sentinel
        Case z; z.seed_join(CHILD_ID, EXIT_OUT);
        z.prov.join_reply = {StartupPpuJoinStatus::Joined, 0};
        z.call_join();
        PCHECK(z.returned && z.get64(EXIT_OUT) == 0 && z.rt->execution.ppu.cpu.gpr[3] == 0 && z.rt->control->ppu_join_returns == 1);
    }
}

void case_join_status_stops() {
    struct Row { const char *name; StartupPpuJoinStatus status; StartupStop stop; };
    const Row rows[] = {
        {"not found", StartupPpuJoinStatus::NotFound, StartupStop::UnsupportedSyscall},
        {"not exited", StartupPpuJoinStatus::NotExited, StartupStop::UnsupportedSyscall},
        {"already joined", StartupPpuJoinStatus::AlreadyJoined, StartupStop::UnsupportedSyscall},
        {"provider failure", StartupPpuJoinStatus::ProviderFailure, StartupStop::ProviderFailure},
        {"out of range status", StartupPpuJoinStatus(99), StartupStop::ProviderFailure}};
    for (const Row &row : rows) {
        Case c; c.seed_join(CHILD_ID, EXIT_OUT);
        c.prov.join_reply = {row.status, EXIT_VALUE};
        c.call_join();
        if (c.obs.stop != row.stop) std::printf("  join row '%s' stop=%d\n", row.name, int(c.obs.stop));
        c.expect_stop(row.stop, __LINE__);
        // Nothing is written on a stop, and no guest errno is invented.
        PCHECK(c.get64(EXIT_OUT) == EXIT_SENTINEL && c.recorder().report().published_count == c.published0);
        PCHECK(c.prov.join_calls == 1);
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_join_calls == 1 && k.ppu_join_returns == 0 && k.ppu_join_has_result && k.ppu_join_last_status == row.status &&
               k.ppu_join_last_exit_value == 0);
    }
}

void case_join_validation() {
    struct Row { const char *name; uint64_t id; uint64_t output; StartupStop stop; };
    const Row rows[] = {
        {"id zero", 0, EXIT_OUT, StartupStop::UnsupportedSyscall},
        {"id high bits", (UINT64_C(1) << 32) | CHILD_ID, EXIT_OUT, StartupStop::UnsupportedSyscall},
        {"output null", CHILD_ID, 0, StartupStop::InvalidMemory},
        {"output high bits", CHILD_ID, (UINT64_C(1) << 32) | EXIT_OUT, StartupStop::InvalidMemory},
        {"output unmapped", CHILD_ID, UNMAPPED, StartupStop::InvalidMemory},
        {"output straddles end", CHILD_ID, 0x7D2000 - 4, StartupStop::InvalidMemory}};
    for (const Row &row : rows) {
        Case c; c.seed_join(row.id, row.output);
        c.prov.join_reply = {StartupPpuJoinStatus::Joined, EXIT_VALUE};   // a valid reply the bridge must never reach
        c.call_join();
        if (c.obs.stop != row.stop) std::printf("  join row '%s' stop=%d\n", row.name, int(c.obs.stop));
        c.expect_stop(row.stop, __LINE__);
        PCHECK(c.prov.join_calls == 0 && c.get64(EXIT_OUT) == EXIT_SENTINEL);
        const StartupControl &k = *c.rt->control;
        PCHECK(k.ppu_join_calls == 1 && k.ppu_join_returns == 0 && !k.ppu_join_has_result && k.ppu_join_last_thread == row.id);
        PCHECK(c.recorder().report().published_count == c.published0);
    }
    {   // Cancelled on entry: the generic syscall boundary stops before the id/output checks or any provider call.
        Case c; c.seed_join(CHILD_ID, EXIT_OUT);
        c.rt->control->cancelled.store(true);
        c.call_join();
        c.expect_stop(StartupStop::Cancelled, __LINE__);
        PCHECK(c.prov.join_calls == 0 && c.get64(EXIT_OUT) == EXIT_SENTINEL);
    }
}

void case_join_trace_capacity() {
    Options o; o.recorder_capacity = 1;   // the join publishes exactly one record; a recorder with no free slot refuses it
    Case c(o); c.seed_join(CHILD_ID, EXIT_OUT);
    // Fill the only slot with a completed run, then join: the exit-value record cannot be reserved.
    c.prov.join_reply = {StartupPpuJoinStatus::Joined, EXIT_VALUE};
    PCHECK(c.recorder().begin(1));
    c.recorder().publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, OPD, 4, 0));
    c.recorder().finish();
    c.call_join();
    c.expect_stop(StartupStop::TraceCapacity, __LINE__);
    // The provider really ran; the exit value was NOT written because its record could not be reserved first.
    PCHECK(c.prov.join_calls == 1 && c.get64(EXIT_OUT) == EXIT_SENTINEL);
    const StartupControl &k = *c.rt->control;
    PCHECK(k.ppu_join_calls == 1 && k.ppu_join_returns == 0 && k.ppu_join_last_status == StartupPpuJoinStatus::Joined);
}
}  // namespace

int main() {
    struct Step { const char *name; void (*fn)(); };
    const Step steps[] = {
        {"missing_services", case_missing_services}, {"success_order", case_success_order}, {"success_boundaries", case_success_boundaries},
        {"validation", case_validation}, {"trace_exhaustion", case_trace_exhaustion}, {"prepare_outcomes", case_prepare_outcomes},
        {"cancellation", case_cancellation}, {"start_failures", case_start_failures},
        {"join_missing_services", case_join_missing_services}, {"join_success", case_join_success},
        {"join_status_stops", case_join_status_stops}, {"join_validation", case_join_validation},
        {"join_trace_capacity", case_join_trace_capacity}};
    for (const Step &s : steps) {
        const int before = create_failures.load() + failures;
        s.fn();
        std::printf("case %-22s %s\n", s.name, create_failures.load() + failures == before ? "ok" : "FAILED");
    }
    const int total = create_failures.load() + failures;
    std::printf("%s failures=%d (HOST test with a provider FIXTURE; not game execution, not a real child)\n",
        total ? "STARTUP_PPU_CREATE_FAIL" : "STARTUP_PPU_CREATE_OK", total);
    return total ? 1 : 0;
}

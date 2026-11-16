/* Focused host test for the four native post-init cellGcmSys queries through the REAL
 * ps3_hle_call dispatcher, the owned StartupExecution, and the native GCM-init service
 * plus RSX front end. The service is attached and the real _cellGcmInitBody dispatch is
 * run first, so every query sees an INITIALIZED service. Bounded host memory only:
 * no GPU, no game, no rendered frame, no Switch build. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "../adapters/guard_pthread.h"
}
#include "gcm_offset_table_expected.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("GCM_QUERY_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};

constexpr uint32_t INIT_NID = 0x15bae46b;
constexpr uint32_t CONFIG_NID = 0xe315a0b2;
constexpr uint32_t LABEL_NID = 0xf80196c1;
constexpr uint32_t OFFSET_TABLE_NID = 0x2922aed0;
constexpr uint32_t CONTROL_NID = 0xa547adde;
constexpr uint32_t FLIP_MODE_NID = 0x4ae8d215;
constexpr uint32_t TILE_INFO_NID = 0xbd100dbc;
constexpr uint32_t BIND_TILE_NID = 0x4524cccd;
constexpr uint32_t BIND_ZCULL_NID = 0x9dc04436;
constexpr uint32_t UNBIND_ZCULL_NID = 0xa75640e8;
constexpr uint32_t UNBIND_TILE_NID = 0xd9b7653e;
constexpr uint32_t DISPLAY_BUFFER_NID = 0xa53d12ae;
constexpr uint64_t DISPLAY_BUFFER_LR = 0x003ab5a8;
constexpr uint64_t BIND_ZCULL_LR = 0x003ab3f0;
constexpr uint64_t UNBIND_ZCULL_LR = 0x003ab268;
constexpr uint64_t UNBIND_TILE_LR = 0x003ab440;
constexpr uint64_t TILE_INFO_LR = 0x003ab518;
constexpr uint64_t BIND_TILE_LR = 0x003ab524;
constexpr uint64_t FLIP_MODE_LR = 0x003aab6c;
constexpr uint64_t INIT_LR = 0x00446b18;
constexpr uint64_t CONFIG_LR = 0x003aa91c;
constexpr uint64_t LABEL_LR = 0x003aa980;
constexpr uint64_t OFFSET_TABLE_LR = 0x003aa9a0;
constexpr uint64_t CONTROL_LR = 0x003aa9b0;
constexpr uint64_t TOC = 0x0078bf78;
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
constexpr uint32_t QUERY_OUT = GCM_WINDOW + 0x8000;
constexpr uint32_t REPORTS = GCM_WINDOW + 0x10000, REPORTS_SIZE = 0x10000;
constexpr uint32_t CONTEXT_END = IO_EA + 0x7ffc;
constexpr uint32_t CMD_SIZE = 0x00010000, IO_SIZE = 0x08500000;

/* The traced firmware GetConfiguration bytes: localAddress, ioAddress, localSize,
 * ioSize, memoryFrequency, coreFrequency (BE u32 each). */
constexpr uint8_t CONFIG_EXPECTED[24] = {
    0xc0,0x00,0x00,0x00, 0x40,0x70,0x00,0x00, 0x0f,0x90,0x00,0x00,
    0x08,0x50,0x00,0x00, 0x26,0xbe,0x36,0x80, 0x1d,0xcd,0x65,0x00};

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

enum class Omit { None, CtxOut, Image, Context, Opd, Control, Reports, OffsetTable, IoTable, Toc, QueryOut, StackSave };

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
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(8192);
    PpuMemoryTraceRecorder recorder;
    Ps3PpuExecutionServices services{};
    gcm_init_service storage{};
    bool attached = false;

    explicit GcmRuntime(Omit omit = Omit::None) {
        guest.add(GCM_WINDOW, GCM_WINDOW_SIZE, 0xa5);
        guest.add(CTX_OUT, 4, 0xa5);
        guest.add(IO_EA, IO_WINDOW, 0xa5);
        guest.add(TOC - 0x69a0, 0x100, 0xa5);
        guest.add(STACK_BASE, STACK_SPAN, 0xa5);
        ranges.push_back({GCM_WINDOW, 16, omit != Omit::Context});
        ranges.push_back({OPD, 8, omit != Omit::Opd});
        ranges.push_back({CONTROL, 12, omit != Omit::Control});
        ranges.push_back({REPORTS, REPORTS_SIZE, omit != Omit::Reports});
        ranges.push_back({OFFSET_TABLE, GCM_INIT_EA_TABLE_BYTES, omit != Omit::OffsetTable});
        ranges.push_back({IO_TABLE, GCM_INIT_IO_TABLE_BYTES, omit != Omit::IoTable});
        ranges.push_back({QUERY_OUT, 0x100, omit != Omit::QueryOut});
        if (omit != Omit::CtxOut) ranges.push_back({CTX_OUT, 4, true});
        if (omit != Omit::Image) ranges.push_back({IO_EA, IO_WINDOW, true});
        ranges.push_back({TOC - 0x69a0, 0x100, true});
        if (omit == Omit::Toc) {
            const uint64_t before = TOC_SLOT - STACK_BASE;
            ranges.push_back({STACK_BASE, before, true});
            ranges.push_back({TOC_SLOT + 8, STACK_SPAN - before - 8, true});
        } else if (omit == Omit::StackSave) {
            // A hole exactly at the zcull parameter-save slots [SP+0x70, SP+0x90).
            const uint64_t before = STACK + 0x70 - STACK_BASE;
            ranges.push_back({STACK_BASE, before, true});
            ranges.push_back({STACK + 0x90, STACK_SPAN - before - 0x20, true});
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
        execution.ppu.cpu.lr = INIT_LR;
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
    void put_u64(uint32_t a, uint64_t v) {
        unsigned char b[8];
        for (int i = 0; i < 8; ++i) b[i] = (unsigned char)(v >> (56 - 8 * i));
        put(a, b, sizeof b);
    }
    uint64_t get(uint32_t a, size_t n) {
        unsigned char b[8]{}; CHECK(mem().read(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK);
        uint64_t v = 0; for (size_t i = 0; i < n; ++i) v = v << 8 | b[i]; return v;
    }
    std::vector<unsigned char> read_bytes(uint32_t a, size_t n) {
        std::vector<unsigned char> b(n);
        CHECK(mem().read(mem().context, a, b.data(), b.size()) == PS3_GUEST_MEMORY_OK);
        return b;
    }
    std::vector<unsigned char> snapshot() {
        std::vector<unsigned char> out;
        for (const auto &r : guest.regions) out.insert(out.end(), r.bytes.begin(), r.bytes.end());
        return out;
    }
    /* The whole guest image with one span (the TOC slot) zeroed, for "no store but the TOC". */
    std::vector<unsigned char> snapshot_ignoring(uint32_t a, size_t n) {
        auto out = snapshot();
        size_t base = 0;
        for (const auto &r : guest.regions) {
            if (a >= r.ea && uint64_t(a) + n <= uint64_t(r.ea) + r.bytes.size()) {
                std::fill(out.begin() + base + (a - r.ea),
                          out.begin() + base + (a - r.ea) + n, (unsigned char)0);
                return out;
            }
            base += r.bytes.size();
        }
        return out;
    }
    bool attach() {
        const gcm_init_layout layout{CONTEXT, OPD, CALLBACK_CODE, TOC, CONTROL, REPORTS,
            REPORTS_SIZE, OFFSET_TABLE, IO_TABLE};
        attached = startup_gcm_init_attach(execution, &storage, layout) == GCM_INIT_OK;
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
    void call_init(uint64_t ctx_out = CTX_OUT, uint64_t cmd = CMD_SIZE, uint64_t io_size = IO_SIZE,
              uint64_t io_ea = IO_EA, uint64_t lr = INIT_LR, uint64_t sp = STACK) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = sp; cpu.gpr[2] = TOC;
        cpu.gpr[3] = ctx_out; cpu.gpr[4] = cmd; cpu.gpr[5] = io_size; cpu.gpr[6] = io_ea;
        cpu.gpr[7] = cpu.gpr[8] = cpu.gpr[9] = UINT64_C(0xffffffffdead0000);
        cpu.lr = lr; cpu.cr = 0x12345678;
        run(INIT_NID, false);
    }
    void call_query(uint32_t nid, uint64_t r3, uint64_t lr, uint64_t sp = STACK) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = sp; cpu.gpr[2] = TOC; cpu.gpr[3] = r3;
        cpu.gpr[4] = cpu.gpr[5] = cpu.gpr[6] = UINT64_C(0xffffffffdead0000);
        cpu.gpr[7] = cpu.gpr[8] = cpu.gpr[9] = UINT64_C(0xffffffffdead0000);
        cpu.lr = lr; cpu.cr = 0x12345678;
        run(nid, false);
    }
    void call_tile_info(uint32_t index, uint32_t location, uint32_t offset, uint32_t size,
                        uint32_t pitch, uint32_t comp, uint32_t base, uint32_t bank,
                        uint64_t lr = TILE_INFO_LR, uint64_t sp = STACK) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = sp; cpu.gpr[2] = TOC;
        cpu.gpr[3] = index; cpu.gpr[4] = location; cpu.gpr[5] = offset; cpu.gpr[6] = size;
        cpu.gpr[7] = pitch; cpu.gpr[8] = comp; cpu.gpr[9] = base; cpu.gpr[10] = bank;
        cpu.lr = lr; cpu.cr = 0x12345678;
        run(TILE_INFO_NID, false);
    }
    /* Seeds the four parameter-save slot words the firmware caller stores (BE 8-byte slots). */
    void seed_zcull_save(uint64_t s9 = 1, uint64_t s10 = 1, uint64_t s11 = 0x80, uint64_t s12 = 0xff) {
        put_u64(STACK + 0x70, s9);
        put_u64(STACK + 0x78, s10);
        put_u64(STACK + 0x80, s11);
        put_u64(STACK + 0x88, s12);
    }
    void call_bind_zcull(uint32_t index, uint32_t offset, uint32_t width, uint32_t height,
                         uint32_t cull_start, uint32_t z_format, uint32_t aa_format,
                         uint32_t zcull_dir, uint64_t s9 = 1, uint64_t s10 = 1,
                         uint64_t s11 = 0x80, uint64_t s12 = 0xff, uint64_t lr = BIND_ZCULL_LR) {
        seed_zcull_save(s9, s10, s11, s12);
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        cpu.gpr[3] = index; cpu.gpr[4] = offset; cpu.gpr[5] = width; cpu.gpr[6] = height;
        cpu.gpr[7] = cull_start; cpu.gpr[8] = z_format; cpu.gpr[9] = aa_format;
        cpu.gpr[10] = zcull_dir;
        cpu.lr = lr; cpu.cr = 0x12345678;
        run(BIND_ZCULL_NID, false);
    }
    void call_set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch, uint32_t width,
                                 uint32_t height, uint64_t lr = DISPLAY_BUFFER_LR) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        cpu.gpr[3] = id; cpu.gpr[4] = offset; cpu.gpr[5] = pitch; cpu.gpr[6] = width;
        cpu.gpr[7] = height;
        cpu.gpr[8] = cpu.gpr[9] = UINT64_C(0xffffffffdead0000);
        cpu.lr = lr; cpu.cr = 0x12345678;
        run(DISPLAY_BUFFER_NID, false);
    }
    bool initialise() {
        if (!attach()) return false;
        call_init();
        return control->stop == StartupStop::None && control->gcm_init_returns == 1;
    }
    size_t published() { return recorder.report().retained_count; }
    std::vector<PpuMemoryTraceEvent> since(size_t from) {
        std::vector<PpuMemoryTraceEvent> out; PpuMemoryTraceSample s;
        for (size_t i = from; recorder.read(i, s); ++i) out.push_back(s.event);
        return out;
    }
};

void config_success_and_exact_effect() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    const auto before = r->read_bytes(QUERY_OUT, 24);
    const size_t from = r->published();
    r->call_query(CONFIG_NID, QUERY_OUT, CONFIG_LR);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.nid == CONFIG_NID && o.lr == CONFIG_LR && o.raw_r3 == QUERY_OUT);
    CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
    CHECK(o.has_result && o.toc_written && o.result == 0);
    CHECK(o.publications == 2 && r->since(from).size() == 2);
    CHECK(r->read_bytes(QUERY_OUT, 24) == std::vector<unsigned char>(CONFIG_EXPECTED, CONFIG_EXPECTED + 24));
    CHECK(r->get(TOC_SLOT, 8) == TOC);
    CHECK(r->control->gcm_config_calls == 1 && r->control->gcm_config_returns == 1);
    CHECK(r->control->gcm_label_calls == 0 && r->control->gcm_offset_table_calls == 0 && r->control->gcm_control_calls == 0);
    (void)before;
}

void label_success_index_and_wrap() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    struct Case { uint32_t index; uint32_t expected; };
    const Case cases[] = {{0, REPORTS}, {1, REPORTS + 0x10}, {255, REPORTS + 0xff0}, {256, REPORTS}};
    for (const auto &c : cases) {
        const size_t from = r->published();
        r->call_query(LABEL_NID, c.index, LABEL_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::None);
        CHECK(r->execution.ppu.cpu.gpr[3] == c.expected && r->execution.ppu.cpu.gpr[2] == TOC);
        CHECK(o.nid == LABEL_NID && o.lr == LABEL_LR && o.raw_r3 == c.index);
        CHECK(o.failure == StartupGcmQueryFailure::None && o.status == GCM_INIT_OK);
        CHECK(o.has_result && o.toc_written && o.result == c.expected && o.publications == 1);
        CHECK(r->since(from).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
        CHECK(r->control->gcm_label_returns == uint64_t(&c - cases) + 1);
    }
    CHECK(r->control->gcm_label_calls == 4 && r->control->gcm_label_returns == 4);
    CHECK(r->control->gcm_config_calls == 0 && r->control->gcm_offset_table_calls == 0 && r->control->gcm_control_calls == 0);
}

void control_success_and_exact_effect() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call_query(CONTROL_NID, UINT64_C(0xffffffffdeadbeef), CONTROL_LR);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == CONTROL && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.nid == CONTROL_NID && o.lr == CONTROL_LR && o.raw_r3 == UINT64_C(0xffffffffdeadbeef));
    CHECK(o.failure == StartupGcmQueryFailure::None && o.status == GCM_INIT_OK);
    CHECK(o.has_result && o.toc_written && o.result == CONTROL && o.publications == 1);
    CHECK(r->since(from).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
    CHECK(r->snapshot() == before);   // control is a pure query: no guest store at all
    CHECK(r->control->gcm_control_calls == 1 && r->control->gcm_control_returns == 1);
}

void offset_table_materialises_once() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    const size_t from = r->published();
    r->call_query(OFFSET_TABLE_NID, QUERY_OUT, OFFSET_TABLE_LR);
    auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == QUERY_OUT && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.nid == OFFSET_TABLE_NID && o.lr == OFFSET_TABLE_LR && o.raw_r3 == QUERY_OUT);
    CHECK(o.failure == StartupGcmQueryFailure::None && o.status == GCM_INIT_OK);
    CHECK(o.has_result && o.toc_written && o.result == QUERY_OUT && o.publications == 301);
    CHECK(r->since(from).size() == 301);
    CHECK(r->get(QUERY_OUT, 8) == (uint64_t(OFFSET_TABLE) << 32 | uint64_t(IO_TABLE)));
    CHECK(r->get(TOC_SLOT, 8) == TOC);
    CHECK(r->read_bytes(OFFSET_TABLE, GCM_INIT_EA_TABLE_BYTES) ==
        std::vector<unsigned char>(gcm_offset_table_ea_to_io_expected,
            gcm_offset_table_ea_to_io_expected + GCM_INIT_EA_TABLE_BYTES));
    CHECK(r->read_bytes(IO_TABLE, GCM_INIT_IO_TABLE_BYTES) ==
        std::vector<unsigned char>(gcm_offset_table_io_to_ea_expected,
            gcm_offset_table_io_to_ea_expected + GCM_INIT_IO_TABLE_BYTES));
    CHECK(r->control->gcm_offset_table_calls == 1 && r->control->gcm_offset_table_returns == 1);
    CHECK(r->control->gcm_config_calls == 0 && r->control->gcm_label_calls == 0 && r->control->gcm_control_calls == 0);

    // The second call writes only the 8-byte output and the TOC return; the tables stay as they are.
    const unsigned char sentinel[16]{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
                                     0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x00};
    r->put(OFFSET_TABLE, sentinel, sizeof sentinel);
    r->put(IO_TABLE, sentinel, sizeof sentinel);
    const size_t from_second = r->published();
    r->call_query(OFFSET_TABLE_NID, QUERY_OUT, OFFSET_TABLE_LR);
    o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == QUERY_OUT && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.has_result && o.toc_written && o.result == QUERY_OUT && o.publications == 2);
    CHECK(r->since(from_second).size() == 2);
    CHECK(r->get(QUERY_OUT, 8) == (uint64_t(OFFSET_TABLE) << 32 | uint64_t(IO_TABLE)));
    CHECK(r->read_bytes(OFFSET_TABLE, sizeof sentinel) == std::vector<unsigned char>(sentinel, sentinel + sizeof sentinel));
    CHECK(r->read_bytes(IO_TABLE, sizeof sentinel) == std::vector<unsigned char>(sentinel, sentinel + sizeof sentinel));
    CHECK(r->control->gcm_offset_table_calls == 2 && r->control->gcm_offset_table_returns == 2);
    CHECK(r->storage.tables_materialized == 1);
}

void before_init_stops_without_store() {
    struct Case { uint32_t nid; uint64_t r3; uint64_t lr; };
    const Case cases[] = {
        {CONFIG_NID, QUERY_OUT, CONFIG_LR},
        {LABEL_NID, 1, LABEL_LR},
        {OFFSET_TABLE_NID, QUERY_OUT, OFFSET_TABLE_LR},
        {CONTROL_NID, 0, CONTROL_LR}};
    for (const auto &c : cases) {
        auto r = std::make_unique<GcmRuntime>();
        CHECK(r->attach());
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_query(c.nid, c.r3, c.lr);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == c.nid && o.failure == StartupGcmQueryFailure::NotInitialized);
        CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == UINT64_C(0x5a5a5a5a5a5a5a5a));
        CHECK(!r->control->gcm_config_returns && !r->control->gcm_label_returns &&
              !r->control->gcm_offset_table_returns && !r->control->gcm_control_returns);
    }
}

void pre_store_failures_leave_snapshot_unchanged() {
    struct Case { const char *name; Omit omit; uint32_t nid; uint64_t r3; StartupGcmQueryFailure failure; StartupStop stop; };
    const Case cases[] = {
        {"config-high-bits", Omit::None, CONFIG_NID, UINT64_C(0x100000000) + QUERY_OUT, StartupGcmQueryFailure::HighBits, StartupStop::GcmQueryStop},
        {"config-unwritable-output", Omit::QueryOut, CONFIG_NID, QUERY_OUT, StartupGcmQueryFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"offset-high-bits", Omit::None, OFFSET_TABLE_NID, UINT64_C(0x100000000) + QUERY_OUT, StartupGcmQueryFailure::HighBits, StartupStop::GcmQueryStop},
        {"offset-unwritable-output", Omit::QueryOut, OFFSET_TABLE_NID, QUERY_OUT, StartupGcmQueryFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"offset-unwritable-ea-table", Omit::OffsetTable, OFFSET_TABLE_NID, QUERY_OUT, StartupGcmQueryFailure::InvalidMemory, StartupStop::InvalidMemory},
        {"offset-unwritable-io-table", Omit::IoTable, OFFSET_TABLE_NID, QUERY_OUT, StartupGcmQueryFailure::InvalidMemory, StartupStop::InvalidMemory}};
    for (const auto &c : cases) {
        auto r = std::make_unique<GcmRuntime>(c.omit);
        CHECK(r->initialise());
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_query(c.nid, c.r3, c.nid == CONFIG_NID ? CONFIG_LR :
            c.nid == OFFSET_TABLE_NID ? OFFSET_TABLE_LR : c.nid == LABEL_NID ? LABEL_LR : CONTROL_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == c.stop);
        CHECK(o.failure == c.failure && !o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(!r->control->gcm_config_returns && !r->control->gcm_label_returns &&
              !r->control->gcm_offset_table_returns && !r->control->gcm_control_returns);
        CHECK(r->storage.tables_materialized == 0);
        (void)c.name;
    }
    // Label/control have no output pointer, but still require a writable TOC slot.
    for (const uint32_t nid : {LABEL_NID, CONTROL_NID}) {
        auto r = std::make_unique<GcmRuntime>();
        CHECK(r->initialise());
        r->ranges.back().writable = false;   // the binding borrows this range array
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_query(nid, nid == LABEL_NID ? 1 : 0, nid == LABEL_NID ? LABEL_LR : CONTROL_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::InvalidMemory);
        CHECK(o.failure == StartupGcmQueryFailure::InvalidMemory);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
    }
}

void missing_service_stops() {
    auto r = std::make_unique<GcmRuntime>();
    const auto before = r->snapshot();
    r->call_query(CONFIG_NID, QUERY_OUT, CONFIG_LR);
    CHECK(r->control->stop == StartupStop::GcmQueryStop);
    CHECK(r->control->gcm_query.failure == StartupGcmQueryFailure::MissingService);
    CHECK(r->snapshot() == before && r->control->gcm_config_calls == 1 && r->control->gcm_config_returns == 0);
}

void flip_mode_records_hsync_and_vsync() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    for (const uint32_t mode : {2u, 1u}) {
        const size_t from = r->published();
        r->call_query(FLIP_MODE_NID, mode, FLIP_MODE_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::None);
        CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
        CHECK(o.nid == FLIP_MODE_NID && o.lr == FLIP_MODE_LR && o.raw_r3 == mode);
        CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
        CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
        CHECK(r->since(from).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
        CHECK(r->storage.flip_mode == mode);
    }
    CHECK(r->control->gcm_flip_mode_calls == 2 && r->control->gcm_flip_mode_returns == 2);
    CHECK(r->control->gcm_config_calls == 0 && r->control->gcm_label_calls == 0 &&
          r->control->gcm_offset_table_calls == 0 && r->control->gcm_control_calls == 0);
}

void flip_mode_rejects_unsupported_modes() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    CHECK(r->storage.flip_mode == 0);
    for (const uint32_t mode : {0u, 3u}) {
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_query(FLIP_MODE_NID, mode, FLIP_MODE_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == FLIP_MODE_NID && o.failure == StartupGcmQueryFailure::ServiceFailed);
        CHECK(o.status == GCM_INIT_UNSUPPORTED_FLIP_MODE);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->storage.flip_mode == 0);
        CHECK(r->control->gcm_flip_mode_returns == 0);
    }
}

void flip_mode_before_init_stops() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call_query(FLIP_MODE_NID, 2, FLIP_MODE_LR);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::GcmQueryStop);
    CHECK(o.nid == FLIP_MODE_NID && o.failure == StartupGcmQueryFailure::NotInitialized);
    CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written && o.publications == 0);
    CHECK(r->since(from).empty() && r->snapshot() == before);
    CHECK(r->get(TOC_SLOT, 8) == UINT64_C(0x5a5a5a5a5a5a5a5a));
    CHECK(r->control->gcm_flip_mode_calls == 1 && r->control->gcm_flip_mode_returns == 0);
}
void tile_info_records_observed_call_and_binds() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    const auto zero = r->snapshot_ignoring(TOC_SLOT, 8);

    // The exact call the game made (0.24.34-vcfg, LR 0x003AB518).
    const size_t from = r->published();
    r->call_tile_info(0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu, 1, 1);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.nid == TILE_INFO_NID && o.lr == TILE_INFO_LR);
    CHECK(o.raw_r3 == 0 && o.raw_r4 == 0 && o.raw_r5 == 0x01010000u &&
          o.raw_r6 == 0x003A0000u && o.raw_r7 == 0x1400u && o.raw_r8 == 0xBu &&
          o.raw_r9 == 1 && o.raw_r10 == 1);
    CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
    CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
    CHECK(r->since(from).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
    CHECK(r->snapshot_ignoring(TOC_SLOT, 8) == zero);   // only the TOC slot was stored
    CHECK(r->control->gcm_tile_info_calls == 1 && r->control->gcm_tile_info_returns == 1);

    const gcm_init_tile *tile = gcm_init_service_tile(&r->storage, 0);
    CHECK(tile != nullptr);
    CHECK(tile->location == 0 && tile->offset == 0x01010000u && tile->size == 0x003A0000u &&
          tile->pitch == 0x1400u && tile->comp == 0xBu && tile->base == 1 && tile->bank == 1);
    CHECK(tile->set == 1 && tile->bound == 0);
    CHECK(gcm_init_service_tile(&r->storage, 15) == nullptr);
    CHECK(gcm_init_service_tile(nullptr, 0) == nullptr);

    // BindTile(0) marks the stored tile bound and writes only the TOC slot.
    const size_t from_bind = r->published();
    r->call_query(BIND_TILE_NID, 0, BIND_TILE_LR);
    const auto &b = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(b.nid == BIND_TILE_NID && b.lr == BIND_TILE_LR && b.raw_r3 == 0);
    CHECK(b.status == GCM_INIT_OK && b.failure == StartupGcmQueryFailure::None);
    CHECK(b.has_result && b.toc_written && b.result == 0 && b.publications == 1);
    CHECK(r->since(from_bind).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
    CHECK(r->snapshot_ignoring(TOC_SLOT, 8) == zero);
    CHECK(tile->set == 1 && tile->bound == 1);
    CHECK(tile->offset == 0x01010000u && tile->size == 0x003A0000u && tile->pitch == 0x1400u &&
          tile->comp == 0xBu && tile->base == 1 && tile->bank == 1);
    CHECK(r->control->gcm_bind_tile_calls == 1 && r->control->gcm_bind_tile_returns == 1);

    // The highest accepted index works too.
    r->call_tile_info(14, 0, 0, 0x10000u, 0x100u, 0, 0, 3);
    CHECK(r->control->stop == StartupStop::None);
    const gcm_init_tile *last = gcm_init_service_tile(&r->storage, 14);
    CHECK(last && last->set == 1 && last->bound == 0 && last->pitch == 0x100u && last->bank == 3);
}

void tile_rejections_leave_state_unchanged() {
    struct Case { const char *name; uint32_t index, location, offset, size, pitch, bank; };
    const Case cases[] = {
        {"index-15", 15, 0, 0x01010000u, 0x003A0000u, 0x1400u, 1},
        {"location-1", 0, 1, 0x01010000u, 0x003A0000u, 0x1400u, 1},
        {"offset-misaligned", 0, 0, 0x01010001u, 0x003A0000u, 0x1400u, 1},
        {"size-misaligned", 0, 0, 0x01010000u, 0x003A0001u, 0x1400u, 1},
        {"size-zero", 0, 0, 0x01010000u, 0u, 0x1400u, 1},
        {"past-local-end", 0, 0, 0x0F800000u, 0x00200000u, 0x1400u, 1},
        {"pitch-zero", 0, 0, 0x01010000u, 0x003A0000u, 0u, 1},
        {"pitch-misaligned", 0, 0, 0x01010000u, 0x003A0000u, 0x1410u, 1},
        {"bank-4", 0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 4}};
    for (const auto &c : cases) {
        auto r = std::make_unique<GcmRuntime>();
        CHECK(r->initialise());
        r->call_tile_info(0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu, 1, 1);
        r->call_query(BIND_TILE_NID, 0, BIND_TILE_LR);
        CHECK(r->control->stop == StartupStop::None);
        const gcm_init_tile kept = *gcm_init_service_tile(&r->storage, 0);
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_tile_info(c.index, c.location, c.offset, c.size, c.pitch, 0xBu, 1, c.bank);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == TILE_INFO_NID && o.failure == StartupGcmQueryFailure::ServiceFailed);
        CHECK(o.status == GCM_INIT_UNSUPPORTED_TILE);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == TOC);
        const gcm_init_tile *tile = gcm_init_service_tile(&r->storage, 0);
        CHECK(tile && tile->set == kept.set && tile->bound == kept.bound &&
              tile->location == kept.location && tile->offset == kept.offset &&
              tile->size == kept.size && tile->pitch == kept.pitch &&
              tile->comp == kept.comp && tile->base == kept.base && tile->bank == kept.bank);
        CHECK(r->control->gcm_tile_info_calls == 2 && r->control->gcm_tile_info_returns == 1);
        (void)c.name;
    }

    // BindTile on an unset tile, and on an out-of-range index.
    for (const uint32_t index : {1u, 15u}) {
        auto r = std::make_unique<GcmRuntime>();
        CHECK(r->initialise());
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_query(BIND_TILE_NID, index, BIND_TILE_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == BIND_TILE_NID && o.failure == StartupGcmQueryFailure::ServiceFailed);
        CHECK(o.status == GCM_INIT_UNSUPPORTED_TILE);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == TOC);   // the init route already published the TOC here
        CHECK(r->control->gcm_bind_tile_calls == 1 && r->control->gcm_bind_tile_returns == 0);
    }
}

void tile_routes_before_init_stop() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call_tile_info(0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu, 1, 1);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == TILE_INFO_NID && o.failure == StartupGcmQueryFailure::NotInitialized);
        CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written &&
              o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->control->gcm_tile_info_calls == 1 && r->control->gcm_tile_info_returns == 0);
    }
    r->call_query(BIND_TILE_NID, 0, BIND_TILE_LR);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == BIND_TILE_NID && o.failure == StartupGcmQueryFailure::NotInitialized);
        CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written &&
              o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == UINT64_C(0x5a5a5a5a5a5a5a5a));
        CHECK(r->control->gcm_bind_tile_calls == 1 && r->control->gcm_bind_tile_returns == 0);
    }
    CHECK(r->storage.state != 1);
}
void zcull_bind_records_observed_call() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    r->seed_zcull_save();   // the caller's parameter save precedes the call
    const auto zero = r->snapshot_ignoring(TOC_SLOT, 8);

    // The exact call the game made (0.24.35-tile, LR 0x003AB3F0) with the four stack slots
    // seeded exactly as the firmware caller stored them: 1, 1, 0x80, 0xff.
    const size_t from = r->published();
    r->call_bind_zcull(0, 0x01010000u, 0x500u, 0x300u, 0, 2, 0, 0, 1, 1, 0x80, 0xff);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::None);
    CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
    CHECK(o.nid == BIND_ZCULL_NID && o.lr == BIND_ZCULL_LR);
    CHECK(o.raw_r3 == 0 && o.raw_r4 == 0x01010000u && o.raw_r5 == 0x500u &&
          o.raw_r6 == 0x300u && o.raw_r7 == 0 && o.raw_r8 == 2 && o.raw_r9 == 0 &&
          o.raw_r10 == 0);
    CHECK(o.raw_stack9 == 1 && o.raw_stack10 == 1 && o.raw_stack11 == 0x80u &&
          o.raw_stack12 == 0xffu);
    CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
    CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
    CHECK(r->since(from).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
    CHECK(r->snapshot_ignoring(TOC_SLOT, 8) == zero);   // only the TOC slot was stored
    CHECK(r->control->gcm_bind_zcull_calls == 1 && r->control->gcm_bind_zcull_returns == 1);

    const gcm_init_zcull *z = gcm_init_service_zcull(&r->storage, 0);
    CHECK(z != nullptr);
    CHECK(z->offset == 0x01010000u && z->width == 0x500u && z->height == 0x300u &&
          z->cull_start == 0 && z->z_format == 2 && z->aa_format == 0 && z->zcull_dir == 0 &&
          z->zcull_format == 1 && z->s_func == 1 && z->s_ref == 0x80u && z->s_mask == 0xffu);
    CHECK(z->set == 1 && z->bound == 1);
    CHECK(gcm_init_service_zcull(&r->storage, 8) == nullptr);
    CHECK(gcm_init_service_zcull(nullptr, 0) == nullptr);
}

void zcull_bind_unreadable_save_slots_stop() {
    auto r = std::make_unique<GcmRuntime>(Omit::StackSave);
    CHECK(r->initialise());
    r->seed_zcull_save();
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call_bind_zcull(0, 0x01010000u, 0x500u, 0x300u, 0, 2, 0, 0);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::InvalidMemory);
    CHECK(o.nid == BIND_ZCULL_NID && o.failure == StartupGcmQueryFailure::InvalidMemory);
    CHECK(!o.has_result && !o.toc_written && o.publications == 0);
    CHECK(r->since(from).empty() && r->snapshot() == before);
    CHECK(r->control->gcm_bind_zcull_calls == 1 && r->control->gcm_bind_zcull_returns == 0);
    const gcm_init_zcull *z = gcm_init_service_zcull(&r->storage, 0);
    CHECK(z && z->set == 0 && z->bound == 0);
}

void zcull_bind_rejections_stop_and_leave_state() {
    struct Case { const char *name; uint32_t index, offset, width, height, cull_start, z_format; };
    const Case cases[] = {
        {"index-8", 8, 0x01010000u, 0x500u, 0x300u, 0, 2},
        {"offset-misaligned", 0, 0x01010001u, 0x500u, 0x300u, 0, 2},
        {"offset-past-local-end", 0, GCM_INIT_LOCAL_SIZE, 0x500u, 0x300u, 0, 2},
        {"width-zero", 0, 0x01010000u, 0, 0x300u, 0, 2},
        {"width-misaligned", 0, 0x01010000u, 0x501u, 0x300u, 0, 2},
        {"width-too-wide", 0, 0x01010000u, 0x1040u, 0x300u, 0, 2},
        {"height-zero", 0, 0x01010000u, 0x500u, 0, 0, 2},
        {"height-misaligned", 0, 0x01010000u, 0x500u, 0x301u, 0, 2},
        {"height-too-tall", 0, 0x01010000u, 0x500u, 0x1040u, 0, 2},
        {"cull-start-misaligned", 0, 0x01010000u, 0x500u, 0x300u, 1, 2},
        {"z-format-0", 0, 0x01010000u, 0x500u, 0x300u, 0, 0},
        {"z-format-3", 0, 0x01010000u, 0x500u, 0x300u, 0, 3}};
    for (const auto &c : cases) {
        auto r = std::make_unique<GcmRuntime>();
        CHECK(r->initialise());
        r->call_bind_zcull(0, 0x01010000u, 0x500u, 0x300u, 0, 2, 0, 0);
        CHECK(r->control->stop == StartupStop::None);
        const gcm_init_zcull kept = *gcm_init_service_zcull(&r->storage, 0);
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_bind_zcull(c.index, c.offset, c.width, c.height, c.cull_start, c.z_format, 0, 0);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == BIND_ZCULL_NID && o.failure == StartupGcmQueryFailure::ServiceFailed);
        CHECK(o.status == GCM_INIT_UNSUPPORTED_ZCULL);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == TOC);   // the first bind already published the TOC here
        const gcm_init_zcull *z = gcm_init_service_zcull(&r->storage, 0);
        CHECK(z && std::memcmp(z, &kept, sizeof kept) == 0);
        CHECK(r->control->gcm_bind_zcull_calls == 2 && r->control->gcm_bind_zcull_returns == 1);
        (void)c.name;
    }
}

void unbind_zcull_and_tile_update_flags() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    r->call_bind_zcull(0, 0x01010000u, 0x500u, 0x300u, 0, 2, 0, 0);
    r->call_tile_info(0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu, 1, 1);
    r->call_query(BIND_TILE_NID, 0, BIND_TILE_LR);
    CHECK(r->control->stop == StartupStop::None);
    const auto zero = r->snapshot_ignoring(TOC_SLOT, 8);

    // UnbindZcull(0): TOC only, clears set and bound of entry 0.
    const size_t from = r->published();
    r->call_query(UNBIND_ZCULL_NID, 0, UNBIND_ZCULL_LR);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::None);
        CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
        CHECK(o.nid == UNBIND_ZCULL_NID && o.lr == UNBIND_ZCULL_LR && o.raw_r3 == 0);
        CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
        CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
        CHECK(r->since(from).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
        CHECK(r->snapshot_ignoring(TOC_SLOT, 8) == zero);
        CHECK(r->control->gcm_unbind_zcull_calls == 1 && r->control->gcm_unbind_zcull_returns == 1);
    }
    const gcm_init_zcull *z = gcm_init_service_zcull(&r->storage, 0);
    CHECK(z && z->set == 0 && z->bound == 0 && z->offset == 0x01010000u &&
          z->width == 0x500u && z->height == 0x300u && z->z_format == 2);

    // UnbindTile(0): TOC only, clears bound and keeps the tile record.
    const size_t from_tile = r->published();
    r->call_query(UNBIND_TILE_NID, 0, UNBIND_TILE_LR);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::None);
        CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
        CHECK(o.nid == UNBIND_TILE_NID && o.lr == UNBIND_TILE_LR && o.raw_r3 == 0);
        CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
        CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
        CHECK(r->since(from_tile).size() == 1 && r->get(TOC_SLOT, 8) == TOC);
        CHECK(r->snapshot_ignoring(TOC_SLOT, 8) == zero);
        CHECK(r->control->gcm_unbind_tile_calls == 1 && r->control->gcm_unbind_tile_returns == 1);
    }
    const gcm_init_tile *tile = gcm_init_service_tile(&r->storage, 0);
    CHECK(tile && tile->set == 1 && tile->bound == 0 && tile->offset == 0x01010000u &&
          tile->size == 0x003A0000u && tile->pitch == 0x1400u);

    // Out-of-range indices are refused and change nothing.
    struct Case { uint32_t nid; uint64_t index; uint64_t lr; gcm_init_status status; };
    const Case bad[] = {
        {UNBIND_ZCULL_NID, 8, UNBIND_ZCULL_LR, GCM_INIT_UNSUPPORTED_ZCULL},
        {UNBIND_ZCULL_NID, 0xFFFFFFFFu, UNBIND_ZCULL_LR, GCM_INIT_UNSUPPORTED_ZCULL},
        {UNBIND_TILE_NID, 15, UNBIND_TILE_LR, GCM_INIT_UNSUPPORTED_TILE},
        {UNBIND_TILE_NID, 0xFFFFFFFFu, UNBIND_TILE_LR, GCM_INIT_UNSUPPORTED_TILE}};
    for (const auto &c : bad) {
        const auto before = r->snapshot();
        const size_t from_bad = r->published();
        r->call_query(c.nid, c.index, c.lr);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == c.nid && o.failure == StartupGcmQueryFailure::ServiceFailed);
        CHECK(o.status == c.status);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from_bad).empty() && r->snapshot() == before);
    }
    CHECK(r->control->gcm_unbind_zcull_calls == 3 && r->control->gcm_unbind_zcull_returns == 1);
    CHECK(r->control->gcm_unbind_tile_calls == 3 && r->control->gcm_unbind_tile_returns == 1);
}

void zcull_and_unbind_routes_before_init_stop() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    r->seed_zcull_save();   // the caller's parameter save precedes the call
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call_bind_zcull(0, 0x01010000u, 0x500u, 0x300u, 0, 2, 0, 0);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == BIND_ZCULL_NID && o.failure == StartupGcmQueryFailure::NotInitialized);
        CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written &&
              o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->control->gcm_bind_zcull_calls == 1 && r->control->gcm_bind_zcull_returns == 0);
    }
    for (const uint32_t nid : {UNBIND_ZCULL_NID, UNBIND_TILE_NID}) {
        r->call_query(nid, 0, nid == UNBIND_ZCULL_NID ? UNBIND_ZCULL_LR : UNBIND_TILE_LR);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == nid && o.failure == StartupGcmQueryFailure::NotInitialized);
        CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written &&
              o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == UINT64_C(0x5a5a5a5a5a5a5a5a));
    }
    CHECK(r->control->gcm_unbind_zcull_calls == 1 && r->control->gcm_unbind_zcull_returns == 0);
    CHECK(r->control->gcm_unbind_tile_calls == 1 && r->control->gcm_unbind_tile_returns == 0);
}
void display_buffer_records_observed_calls() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->initialise());
    const auto zero = r->snapshot_ignoring(TOC_SLOT, 8);
    const size_t from = r->published();

    // The first observed call (0.24.36-zcul, LR 0x003AB5A8): id 0, offset 0x013B0000,
    // pitch 0x1400, 1280x720.
    r->call_set_display_buffer(0, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::None);
        CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
        CHECK(o.nid == DISPLAY_BUFFER_NID && o.lr == DISPLAY_BUFFER_LR);
        CHECK(o.raw_r3 == 0 && o.raw_r4 == 0x013B0000u && o.raw_r5 == 0x1400u &&
              o.raw_r6 == 0x500u && o.raw_r7 == 0x2D0u);
        CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
        CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
        CHECK(r->get(TOC_SLOT, 8) == TOC);
        CHECK(r->since(from).size() == 1);   // only the TOC slot was stored
    }
    // The second observed call: id 1, offset 0x01734000, same pitch and dimensions.
    r->call_set_display_buffer(1, 0x01734000u, 0x1400u, 0x500u, 0x2D0u);
    {
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::None);
        CHECK(r->execution.ppu.cpu.gpr[3] == 0 && r->execution.ppu.cpu.gpr[2] == TOC);
        CHECK(o.nid == DISPLAY_BUFFER_NID && o.lr == DISPLAY_BUFFER_LR);
        CHECK(o.raw_r3 == 1 && o.raw_r4 == 0x01734000u && o.raw_r5 == 0x1400u &&
              o.raw_r6 == 0x500u && o.raw_r7 == 0x2D0u);
        CHECK(o.status == GCM_INIT_OK && o.failure == StartupGcmQueryFailure::None);
        CHECK(o.has_result && o.toc_written && o.result == 0 && o.publications == 1);
        CHECK(r->get(TOC_SLOT, 8) == TOC);
        CHECK(r->since(from).size() == 2);   // two TOC stores, no other guest byte
    }
    CHECK(r->snapshot_ignoring(TOC_SLOT, 8) == zero);
    CHECK(r->control->gcm_display_buffer_calls == 2 &&
          r->control->gcm_display_buffer_returns == 2);

    const gcm_init_display_buffer *b0 = gcm_init_service_display_buffer(&r->storage, 0);
    CHECK(b0 != nullptr);
    CHECK(b0->offset == 0x013B0000u && b0->pitch == 0x1400u && b0->width == 0x500u &&
          b0->height == 0x2D0u && b0->set == 1);
    const gcm_init_display_buffer *b1 = gcm_init_service_display_buffer(&r->storage, 1);
    CHECK(b1 != nullptr);
    CHECK(b1->offset == 0x01734000u && b1->pitch == 0x1400u && b1->width == 0x500u &&
          b1->height == 0x2D0u && b1->set == 1);
    CHECK(gcm_init_service_display_buffer(&r->storage, 2)->set == 0);
    CHECK(gcm_init_service_display_buffer(&r->storage, 8) == nullptr);
    CHECK(gcm_init_service_display_buffer(nullptr, 0) == nullptr);
}

void display_buffer_rejections_stop_without_store() {
    struct Case { const char *name; uint32_t id, offset, pitch, width, height; };
    const Case cases[] = {
        {"id-8", 8, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u},
        {"id-max", 0xFFFFFFFFu, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u},
        {"width-zero", 0, 0x013B0000u, 0x1400u, 0u, 0x2D0u},
        {"width-4097", 0, 0x013B0000u, 0x1400u, 4097u, 0x2D0u},
        {"height-zero", 0, 0x013B0000u, 0x1400u, 0x500u, 0u},
        {"height-4097", 0, 0x013B0000u, 0x1400u, 0x500u, 4097u},
        {"pitch-zero", 0, 0x013B0000u, 0u, 0x500u, 0x2D0u},
        {"pitch-misaligned", 0, 0x013B0000u, 0x1410u, 0x500u, 0x2D0u},
        {"offset-misaligned", 0, 0x013B0001u, 0x1400u, 0x500u, 0x2D0u},
        {"past-local-end", 0, 0x0F800100u, 0x100u, 0x500u, 4096u}};
    for (const auto &c : cases) {
        auto r = std::make_unique<GcmRuntime>();
        CHECK(r->initialise());
        r->call_set_display_buffer(0, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u);
        r->call_set_display_buffer(7, 0, 0x40u, 1, 1);
        CHECK(r->control->stop == StartupStop::None);
        const gcm_init_display_buffer kept0 = *gcm_init_service_display_buffer(&r->storage, 0);
        const gcm_init_display_buffer kept7 = *gcm_init_service_display_buffer(&r->storage, 7);
        const auto before = r->snapshot();
        const size_t from = r->published();
        r->call_set_display_buffer(c.id, c.offset, c.pitch, c.width, c.height);
        const auto &o = r->control->gcm_query;
        CHECK(r->control->stop == StartupStop::GcmQueryStop);
        CHECK(o.nid == DISPLAY_BUFFER_NID && o.failure == StartupGcmQueryFailure::ServiceFailed);
        CHECK(o.status == GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER);
        CHECK(!o.has_result && !o.toc_written && o.publications == 0);
        CHECK(r->since(from).empty() && r->snapshot() == before);
        CHECK(r->get(TOC_SLOT, 8) == TOC);   // the init route already published the TOC here
        const gcm_init_display_buffer *b0 = gcm_init_service_display_buffer(&r->storage, 0);
        const gcm_init_display_buffer *b7 = gcm_init_service_display_buffer(&r->storage, 7);
        CHECK(b0 && std::memcmp(b0, &kept0, sizeof kept0) == 0);
        CHECK(b7 && std::memcmp(b7, &kept7, sizeof kept7) == 0);
        CHECK(r->control->gcm_display_buffer_calls == 3 &&
              r->control->gcm_display_buffer_returns == 2);
        (void)c.name;
    }
}

void display_buffer_route_before_init_stops() {
    auto r = std::make_unique<GcmRuntime>();
    CHECK(r->attach());
    const auto before = r->snapshot();
    const size_t from = r->published();
    r->call_set_display_buffer(0, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u);
    const auto &o = r->control->gcm_query;
    CHECK(r->control->stop == StartupStop::GcmQueryStop);
    CHECK(o.nid == DISPLAY_BUFFER_NID && o.failure == StartupGcmQueryFailure::NotInitialized);
    CHECK(o.status == GCM_INIT_NOT_INITIALIZED && !o.has_result && !o.toc_written &&
          o.publications == 0);
    CHECK(r->since(from).empty() && r->snapshot() == before);
    CHECK(r->get(TOC_SLOT, 8) == UINT64_C(0x5a5a5a5a5a5a5a5a));
    CHECK(r->control->gcm_display_buffer_calls == 1 &&
          r->control->gcm_display_buffer_returns == 0);
    CHECK(gcm_init_service_display_buffer(&r->storage, 0)->set == 0);
}
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }

int main() {
    config_success_and_exact_effect();
    label_success_index_and_wrap();
    control_success_and_exact_effect();
    offset_table_materialises_once();
    before_init_stops_without_store();
    pre_store_failures_leave_snapshot_unchanged();
    missing_service_stops();
    flip_mode_records_hsync_and_vsync();
    flip_mode_rejects_unsupported_modes();
    flip_mode_before_init_stops();
    tile_info_records_observed_call_and_binds();
    tile_rejections_leave_state_unchanged();
    tile_routes_before_init_stop();
    zcull_bind_records_observed_call();
    zcull_bind_unreadable_save_slots_stop();
    zcull_bind_rejections_stop_and_leave_state();
    unbind_zcull_and_tile_update_flags();
    zcull_and_unbind_routes_before_init_stop();
    display_buffer_records_observed_calls();
    display_buffer_rejections_stop_without_store();
    display_buffer_route_before_init_stops();
    std::printf("STARTUP_GCM_QUERY_FAILURES=%d\n", failures);
    return failures ? 1 : 0;
}

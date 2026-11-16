/* Focused host test for the native game-data imports in startup_execution.cpp:
 *   cellGameDataCheckCreate2 (NID 0xC9645C41), cellGameDataGetSizeKB (NID 0xA9B0C1D9)
 *   and child-only sys_ppu_thread_exit (NID 0xAFF080A4).
 * The REAL ps3_hle_call dispatcher, the owned StartupExecution, the domain game-data service
 * (native/domain/gamedata_service.c) and a real compiled-code dispatch are exercised against an
 * in-memory storage port. The guest is a label fixture; no game, GPU, SD or network. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "../adapters/guard_pthread.h"
}
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("GAMEDATA_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};

constexpr uint32_t CHECK_NID = 0xC9645C41u;
constexpr uint32_t SIZE_NID = 0xA9B0C1D9u;
constexpr uint32_t EXIT_NID = 0xAFF080A4u;
constexpr uint32_t WINDOW = 0x0e100000u, WINDOW_SIZE = 0x40000u;
constexpr uint32_t STACK_BASE = 0xd0000000u, STACK_SPAN = 0x5000u;
constexpr uint32_t STACK = STACK_BASE + 0x3000u;
constexpr uint32_t TOC_SLOT = STACK + 0x28u;
constexpr uint32_t FRAME = STACK - GAMEDATA_FIRMWARE_FRAME;
constexpr uint32_t OPD = WINDOW + 0x0100u;
constexpr uint32_t PARAM = WINDOW + 0x1000u;
constexpr uint32_t SIZE_OUT = WINDOW + 0x2000u;
constexpr uint32_t DIR = WINDOW + 0x3000u;
constexpr uint32_t TOC = 0x0078bf78u;
constexpr uint32_t CALLBACK_TOC = 0x0078bf78u;
constexpr uint32_t ENTRY = 0x00510a40u;
constexpr uint64_t CALLER_LR = 0x003ab5a8u;
constexpr uint64_t CALLER_CTR = 0x003ab5acu;
constexpr uint64_t TOC_SENTINEL = UINT64_C(0x5a5a5a5a5a5a5a5a);
constexpr const char *DIR_NAME = "BCUS98123";

std::array<uint8_t, GAMEDATA_PARAM_BYTES> make_param()
{
    std::array<uint8_t, GAMEDATA_PARAM_BYTES> p{};
    std::memcpy(p.data() + GAMEDATA_PARAM_TITLE, "INFAMOUS G2", 12);
    std::memcpy(p.data() + GAMEDATA_PARAM_TITLE + 0x80, "Localized Title", 16);
    std::memcpy(p.data() + GAMEDATA_PARAM_TITLE_ID, "BCUS98123", 10);
    std::memcpy(p.data() + GAMEDATA_PARAM_DATA_VERSION, "01.00", 6);
    p[GAMEDATA_PARAM_PARENTAL_LEVEL + 0] = 0; p[GAMEDATA_PARAM_PARENTAL_LEVEL + 1] = 0;
    p[GAMEDATA_PARAM_PARENTAL_LEVEL + 2] = 0; p[GAMEDATA_PARAM_PARENTAL_LEVEL + 3] = 5;
    p[GAMEDATA_PARAM_ATTRIBUTE + 0] = 0; p[GAMEDATA_PARAM_ATTRIBUTE + 1] = 0;
    p[GAMEDATA_PARAM_ATTRIBUTE + 2] = 0; p[GAMEDATA_PARAM_ATTRIBUTE + 3] = 1;
    return p;
}

struct FakeStorage {
    std::string dir_name;
    bool dir_exists = false, usrdir_created = false;
    uint64_t free_kb = 100000, size_kb = 0;
    int64_t times[3]{11, 12, 13};
    std::string file_name;
    std::vector<uint8_t> file;
    gamedata_storage_status fail_free = GAMEDATA_STORAGE_OK;
    gamedata_storage_status fail_stat = GAMEDATA_STORAGE_OK;
    gamedata_storage_status fail_read = GAMEDATA_STORAGE_OK;
    gamedata_storage_status fail_create = GAMEDATA_STORAGE_OK;
    gamedata_storage_status fail_write = GAMEDATA_STORAGE_OK;
    unsigned creates = 0, writes = 0, reads = 0;

    static FakeStorage *self(void *context) { return static_cast<FakeStorage *>(context); }
    static gamedata_storage_status free_kb_fn(void *context, uint64_t *out)
    {
        FakeStorage &s = *self(context);
        if (s.fail_free != GAMEDATA_STORAGE_OK) return s.fail_free;
        if (!out) return GAMEDATA_STORAGE_IO_ERROR;
        *out = s.free_kb;
        return GAMEDATA_STORAGE_OK;
    }
    static gamedata_storage_status stat_dir_fn(void *context, const char *dir, uint64_t *size, int64_t t[3])
    {
        FakeStorage &s = *self(context);
        if (s.fail_stat != GAMEDATA_STORAGE_OK) return s.fail_stat;
        if (!dir || !size || !t || s.dir_name != dir || !s.dir_exists) return GAMEDATA_STORAGE_NOT_FOUND;
        *size = s.size_kb;
        t[0] = s.times[0]; t[1] = s.times[1]; t[2] = s.times[2];
        return GAMEDATA_STORAGE_OK;
    }
    static gamedata_storage_status read_file_fn(void *context, const char *dir, const char *name,
                                                uint8_t *buf, size_t cap, size_t *len)
    {
        FakeStorage &s = *self(context);
        ++s.reads;
        if (s.fail_read != GAMEDATA_STORAGE_OK) return s.fail_read;
        if (!dir || !name || !buf || !len || s.dir_name != dir || !s.dir_exists ||
            s.file_name != name || s.file.empty()) return GAMEDATA_STORAGE_NOT_FOUND;
        if (s.file.size() > cap) return GAMEDATA_STORAGE_IO_ERROR;
        std::memcpy(buf, s.file.data(), s.file.size());
        *len = s.file.size();
        return GAMEDATA_STORAGE_OK;
    }
    static gamedata_storage_status create_dir_fn(void *context, const char *dir)
    {
        FakeStorage &s = *self(context);
        ++s.creates;
        if (s.fail_create != GAMEDATA_STORAGE_OK) return s.fail_create;
        if (!dir || s.dir_name != dir) return GAMEDATA_STORAGE_IO_ERROR;
        s.dir_exists = true;
        s.usrdir_created = true;
        return GAMEDATA_STORAGE_OK;
    }
    static gamedata_storage_status write_file_fn(void *context, const char *dir, const char *name,
                                                 const uint8_t *buf, size_t len)
    {
        FakeStorage &s = *self(context);
        ++s.writes;
        if (s.fail_write != GAMEDATA_STORAGE_OK) return s.fail_write;
        if (!dir || !name || !buf || s.dir_name != dir || !s.dir_exists)
            return GAMEDATA_STORAGE_IO_ERROR;
        s.file_name = name;
        s.file.assign(buf, buf + len);
        return GAMEDATA_STORAGE_OK;
    }
    gamedata_storage_port port()
    {
        gamedata_storage_port p{};
        p.context = this;
        p.free_kb = &free_kb_fn;
        p.stat_dir = &stat_dir_fn;
        p.read_file = &read_file_fn;
        p.create_dir = &create_dir_fn;
        p.write_file = &write_file_fn;
        return p;
    }
};

struct Guest {
    struct Region { uint32_t ea; std::vector<unsigned char> bytes; };
    std::vector<Region> regions;
    Region &add(uint32_t ea, size_t size, unsigned char fill = 0)
    {
        regions.push_back(Region{ea, std::vector<unsigned char>(size, fill)});
        return regions.back();
    }
    Region *find(uint32_t ea, size_t length)
    {
        for (Region &r : regions)
            if (ea >= r.ea && uint64_t(ea) + length <= uint64_t(r.ea) + r.bytes.size()) return &r;
        return nullptr;
    }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n)
    { return static_cast<Guest *>(p)->find(a, size_t(n)) ? PS3_GUEST_MEMORY_OK : PS3_GUEST_MEMORY_INVALID_RANGE; }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n)
    { return static_cast<Guest *>(p)->find(a, size_t(n)) ? PS3_GUEST_MEMORY_OK : PS3_GUEST_MEMORY_INVALID_RANGE; }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n)
    {
        auto *r = static_cast<Guest *>(p)->find(a, size_t(n));
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memset(r->bytes.data() + (a - r->ea), 0, size_t(n));
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status read(void *p, uint32_t a, void *v, size_t n)
    {
        auto *r = static_cast<Guest *>(p)->find(a, n);
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memcpy(v, r->bytes.data() + (a - r->ea), n);
        return PS3_GUEST_MEMORY_OK;
    }
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *v, size_t n)
    {
        auto *r = static_cast<Guest *>(p)->find(a, n);
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memcpy(r->bytes.data() + (a - r->ea), v, n);
        return PS3_GUEST_MEMORY_OK;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};

struct Lookup {
    uint32_t known = ENTRY;
    ps3_compiled_function target = nullptr;
    unsigned calls = 0;
    ps3_compiled_code_lookup port{this, &Lookup::find};
    static ps3_compiled_code_status find(const void *context, uint32_t address, ps3_compiled_function *out)
    {
        Lookup &l = *const_cast<Lookup *>(static_cast<const Lookup *>(context));
        ++l.calls;
        if (address != l.known || !l.target) return PS3_COMPILED_CODE_NOT_FOUND;
        *out = l.target;
        return PS3_COMPILED_CODE_OK;
    }
};

enum class Variant { Ok, NoSpace, Cancel, TailCall };
struct Runtime;

struct CallbackState {
    Runtime *rt = nullptr;
    Variant variant = Variant::Ok;
    bool invoked = false, args_ok = false;
    bool tail_invoked = false;
    bool saw_new = false;
    int32_t saw_size = 0, saw_free = 0;
    std::string content_path, game_path;
    std::array<uint8_t, GAMEDATA_PARAM_BYTES> setparam{};
};
CallbackState *g_callback = nullptr;

struct Runtime {
    Guest guest;
    ps3_pthread_guard guard{};
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[16]{};
    ps3_atomic_pin_record pins[128]{};
    std::vector<PpuMemoryRamRange> ranges;
    std::unique_ptr<PpuMemoryBinding> binding;
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(16384);
    PpuMemoryTraceRecorder recorder;
    Ps3PpuExecutionServices services{};
    Lookup lookup;
    FakeStorage storage;
    StartupGameDataPort port{};
    gamedata_check state{};

    explicit Runtime(bool existing = false, bool frame_writable = true, Variant variant = Variant::Ok)
    {
        guest.add(WINDOW, WINDOW_SIZE, 0xa5);
        guest.add(STACK_BASE, STACK_SPAN, 0xa5);
        ranges.push_back({WINDOW, WINDOW_SIZE, true});
        if (frame_writable) {
            ranges.push_back({STACK_BASE, STACK_SPAN, true});
        } else {
            ranges.push_back({STACK_BASE, FRAME - STACK_BASE, true});
            ranges.push_back({FRAME, STACK - FRAME, false});
            ranges.push_back({STACK, STACK_BASE + STACK_SPAN - STACK, true});
        }
        CHECK(!ps3_pthread_guard_init(&guard));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), ps3_pthread_guard_port(&guard),
            owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges.data(), ranges.size()));
        services.memory = &binding->coherent_memory;
        services.compiled_code = &lookup.port;
        lookup.target = &callback_entry;
        execution.control = control.get();
        execution.process_sdk = {0x36ffffu, true};
        execution.ppu.cpu.thread_id = 1;
        execution.ppu.cpu.gpr[1] = STACK;
        execution.ppu.cpu.gpr[2] = TOC;
        execution.ppu.cpu.lr = CALLER_LR;
        execution.ppu.cpu.ctr = CALLER_CTR;
        execution.dispatch_stop = {this, &Runtime::dispatch_stop};
        storage.dir_name = DIR_NAME;
        storage.size_kb = 42;
        if (existing) {
            storage.dir_exists = true;
            storage.file_name = "PARAM.SFO";
            const auto param = make_param();
            storage.file.resize(GAMEDATA_SFO_MAX_BYTES);
            const size_t n = gamedata_param_sfo_encode(param.data(), storage.file.data(), storage.file.size());
            CHECK(n != 0);
            storage.file.resize(n);
        }
        port.storage = storage.port();
        port.state = &state;
        execution.gamedata = &port;
        (void)variant;
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Strict, 0));
        CHECK(native_startup_admit_recorded(execution, *binding, services, recorder));
        put32(OPD, ENTRY);
        put32(OPD + 4, CALLBACK_TOC);
        std::memset(name_buffer, 0, sizeof(name_buffer));
        std::strcpy(name_buffer, DIR_NAME);
        put(DIR, name_buffer, std::strlen(DIR_NAME) + 1);
        put_u64(TOC_SLOT, TOC_SENTINEL);
        put32(SIZE_OUT, 0xdeadbeefu);
    }
    ~Runtime()
    {
        if (execution.ppu.admitted) CHECK(ppu_memory_release_after_join(execution.ppu) == PS3_GUEST_MEMORY_OK);
        ps3_pthread_guard_destroy(&guard);
    }
    static ps3_compiled_dispatch_stop_reason dispatch_stop(void *) { return PS3_COMPILED_DISPATCH_CONTINUE; }
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n)
    { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    void put32(uint32_t a, uint32_t v)
    {
        const unsigned char b[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        put(a, b, 4);
    }
    void put_u64(uint32_t a, uint64_t v)
    {
        unsigned char b[8]{};
        for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (56 - 8 * i));
        put(a, b, 8);
    }
    uint32_t get32(uint32_t a)
    {
        unsigned char b[4]{};
        CHECK(mem().read(mem().context, a, b, 4) == PS3_GUEST_MEMORY_OK);
        return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    }
    uint64_t get_u64(uint32_t a)
    {
        unsigned char b[8]{};
        CHECK(mem().read(mem().context, a, b, 8) == PS3_GUEST_MEMORY_OK);
        uint64_t v = 0;
        for (unsigned char c : b) v = v << 8 | c;
        return v;
    }
    std::vector<unsigned char> snapshot()
    {
        std::vector<unsigned char> out;
        for (const auto &r : guest.regions) out.insert(out.end(), r.bytes.begin(), r.bytes.end());
        return out;
    }
    void run(uint32_t nid)
    {
        ppu_context &cpu = execution.ppu.cpu;
        control->stop = StartupStop::None;
        active = control.get();
        control->cpu = &cpu;
        try { ps3_hle_call(nid, &cpu); active = nullptr; }
        catch (const Terminal &) { active = nullptr; }
    }
    void seed(uint32_t version, uint32_t dir, uint32_t dialog, uint32_t opd, uint32_t container)
    {
        ppu_context &cpu = execution.ppu.cpu;
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC;
        cpu.gpr[3] = version; cpu.gpr[4] = dir; cpu.gpr[5] = dialog;
        cpu.gpr[6] = opd; cpu.gpr[7] = container;
        cpu.lr = CALLER_LR; cpu.ctr = CALLER_CTR; cpu.cr = 0x12345678;
    }
    void call_check(uint32_t version = 0, uint32_t dir = DIR, uint32_t dialog = 1,
                    uint32_t opd = OPD, uint32_t container = 0x1234)
    {
        seed(version, dir, dialog, opd, container);
        run(CHECK_NID);
    }
    void call_size(uint32_t out = SIZE_OUT)
    {
        ppu_context &cpu = execution.ppu.cpu;
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC; cpu.gpr[3] = out;
        cpu.lr = CALLER_LR; cpu.ctr = CALLER_CTR;
        run(SIZE_NID);
    }
    void call_exit(uint64_t value)
    {
        ppu_context &cpu = execution.ppu.cpu;
        cpu.gpr[1] = STACK; cpu.gpr[2] = TOC; cpu.gpr[3] = value;
        cpu.lr = CALLER_LR; cpu.ctr = CALLER_CTR;
        run(EXIT_NID);
    }
    void setup_callback(CallbackState &cb, Variant variant)
    {
        cb = {};
        cb.rt = this;
        cb.variant = variant;
        g_callback = &cb;
    }
    static void callback_entry(ppu_context *cpu);
    static void tail_entry(void *cpu);
    char name_buffer[64]{};
};

void Runtime::callback_entry(ppu_context *cpu)
{
    CallbackState &cb = *g_callback;
    Runtime &r = *cb.rt;
    cb.invoked = true;
    const uint32_t frame = FRAME;
    const uint32_t cb_result = frame + GAMEDATA_FRAME_CBRESULT;
    const uint32_t stat_get = frame + GAMEDATA_FRAME_STATGET;
    const uint32_t stat_set = frame + GAMEDATA_FRAME_STATSET;
    const bool args = cpu->gpr[1] == frame && cpu->gpr[2] == CALLBACK_TOC &&
        cpu->gpr[3] == cb_result && cpu->gpr[4] == stat_get && cpu->gpr[5] == stat_set;
    const bool backchain = r.get_u64(frame) == frame + GAMEDATA_FIRMWARE_FRAME;
    cb.saw_new = r.get32(stat_get + GAMEDATA_STATGET_IS_NEW_DATA) != 0;
    cb.saw_size = int32_t(r.get32(stat_get + GAMEDATA_STATGET_SIZE_KB));
    cb.saw_free = int32_t(r.get32(stat_get + GAMEDATA_STATGET_HDD_FREE_SIZE_KB));
    auto read_string = [&r](uint32_t address, size_t cap) {
        std::string s;
        for (size_t i = 0; i < cap; ++i) {
            unsigned char c = 0;
            if (r.mem().read(r.mem().context, address + uint32_t(i), &c, 1) != PS3_GUEST_MEMORY_OK) break;
            if (!c) break;
            s.push_back(char(c));
        }
        return s;
    };
    cb.content_path = read_string(stat_get + GAMEDATA_STATGET_CONTENT_INFO_PATH,
        GAMEDATA_STATGET_CONTENT_INFO_PATH_BYTES);
    cb.game_path = read_string(stat_get + GAMEDATA_STATGET_GAME_DATA_PATH,
        GAMEDATA_STATGET_GAME_DATA_PATH_BYTES);
    cb.setparam = make_param();
    cb.args_ok = args && backchain;
    switch (cb.variant) {
    case Variant::Ok:
        r.put32(cb_result, 0);
        r.put(PARAM, cb.setparam.data(), cb.setparam.size());
        r.put32(stat_set, PARAM);
        break;
    case Variant::NoSpace:
        r.put32(cb_result, uint32_t(-1));
        r.put32(cb_result + 4, 777);
        break;
    case Variant::Cancel:
        r.put32(cb_result, 1);
        break;
    case Variant::TailCall:
        // A tail-calling funcStat: it records the pending tail call and RETURNS without writing CBResult. Only the caller's drain
        // (the generated DRAIN_TRAMPOLINE loop, which the native HLE must run) invokes tail_entry, which performs the write. Without
        // the drain CheckCreate2 would read the zeroed CBResult and report "OK, no setParam": no directory would be created.
        g_trampoline_fn = &Runtime::tail_entry;
        break;
    }
}

void Runtime::tail_entry(void *context)
{
    CallbackState &cb = *g_callback;
    Runtime &r = *cb.rt;
    cb.tail_invoked = true;
    CHECK(context == &r.execution.ppu.cpu);   // the SAME cpu: the tail call runs at its caller
    const uint32_t frame = FRAME;
    const uint32_t cb_result = frame + GAMEDATA_FRAME_CBRESULT;
    const uint32_t stat_get = frame + GAMEDATA_FRAME_STATGET;
    const uint32_t stat_set = frame + GAMEDATA_FRAME_STATSET;
    CHECK(r.get32(stat_get + GAMEDATA_STATGET_IS_NEW_DATA) != 0);
    r.put32(cb_result, 0);
    r.put(PARAM, cb.setparam.data(), cb.setparam.size());
    r.put32(stat_set, PARAM);
}

void new_data_ok_flow()
{
    Runtime r;
    CallbackState cb;
    r.setup_callback(cb, Variant::Ok);
    r.call_check();
    const auto &o = r.control->gamedata_check;
    CHECK(r.control->stop == StartupStop::None);
    CHECK(cb.invoked && cb.args_ok);
    CHECK(cb.saw_new && cb.saw_free == 100000);
    CHECK(cb.content_path == "/dev_hdd0/game/BCUS98123");
    CHECK(cb.game_path == "/dev_hdd0/game/BCUS98123/USRDIR");
    CHECK(r.execution.ppu.cpu.gpr[3] == 0);
    CHECK(r.execution.ppu.cpu.gpr[1] == STACK);
    CHECK(r.execution.ppu.cpu.gpr[2] == TOC);
    CHECK(r.execution.ppu.cpu.lr == CALLER_LR);
    CHECK(r.execution.ppu.cpu.ctr == CALLER_CTR);
    CHECK(r.get_u64(TOC_SLOT) == TOC);
    CHECK(o.has_result && o.toc_written && o.callback_invoked && o.dispatch_returned);
    CHECK(o.result == 0 && o.status == GAMEDATA_OK);
    CHECK(o.action.action == GAMEDATA_ACTION_CREATED_OR_UPDATED);
    CHECK(o.frame == FRAME && o.cb_result == FRAME + GAMEDATA_FRAME_CBRESULT);
    CHECK(o.stat_get == FRAME + GAMEDATA_FRAME_STATGET && o.stat_set == FRAME + GAMEDATA_FRAME_STATSET);
    CHECK(o.set_param == PARAM && o.check.is_new == 1 && o.check.size_kb == 0);
    CHECK(r.storage.dir_exists && r.storage.usrdir_created);
    CHECK(r.storage.file_name == "PARAM.SFO" && !r.storage.file.empty());
    std::array<uint8_t, GAMEDATA_PARAM_BYTES> decoded{};
    CHECK(gamedata_param_sfo_decode(r.storage.file.data(), r.storage.file.size(), decoded.data()) == GAMEDATA_OK);
    CHECK(decoded == cb.setparam);
    CHECK(r.control->gamedata_check_calls == 1 && r.control->gamedata_check_returns == 1);
}

void existing_data_and_size()
{
    Runtime r(true);
    CallbackState cb;
    r.setup_callback(cb, Variant::Cancel);
    r.call_check();
    const auto &o = r.control->gamedata_check;
    CHECK(r.control->stop == StartupStop::None);
    CHECK(cb.invoked && cb.args_ok && !cb.saw_new && cb.saw_size == 42);
    CHECK(o.result == 1 && o.action.action == GAMEDATA_ACTION_NONE);
    CHECK(o.check.is_new == 0 && o.check.size_kb == 42);
    CHECK(r.storage.creates == 0 && r.storage.writes == 0);
    r.call_size();
    const auto &s = r.control->gamedata_size;
    CHECK(r.control->stop == StartupStop::None);
    CHECK(r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[2] == TOC);
    CHECK(s.has_result && s.written && s.toc_written && s.value == 42);
    CHECK(r.get32(SIZE_OUT) == 42);
    CHECK(r.control->gamedata_size_calls == 1 && r.control->gamedata_size_returns == 1);
}

void error_variants_create_nothing()
{
    for (Variant v : {Variant::NoSpace, Variant::Cancel}) {
        Runtime r;
        CallbackState cb;
        r.setup_callback(cb, v);
        r.call_check();
        const auto &o = r.control->gamedata_check;
        CHECK(r.control->stop == StartupStop::None);
        CHECK(cb.invoked && cb.args_ok);
        CHECK(r.execution.ppu.cpu.gpr[3] == 0);
        CHECK(r.storage.creates == 0 && r.storage.writes == 0 && r.storage.file.empty());
        if (v == Variant::NoSpace) {
            CHECK(o.result == -1 && o.err_need_size_kb == 777);
            CHECK(o.action.action == GAMEDATA_ACTION_CBRESULT_ERROR);
        } else {
            CHECK(o.result == 1 && o.action.action == GAMEDATA_ACTION_NONE);
        }
    }
}

void argument_quirks()
{
    Runtime r;
    CallbackState cb;
    r.setup_callback(cb, Variant::Ok);
    const std::string too_long(33, 'A');
    CHECK(too_long.size() == 33);
    r.put32(DIR + 0x100, 0);
    r.put(DIR + 0x100, too_long.c_str(), too_long.size() + 1);
    struct Case { const char *name; uint32_t version, dir, func; uint64_t expected; };
    const Case cases[] = {
        {"version", 5, DIR, OPD, 5},
        {"null-dir", 0, 0, OPD, 0},
        {"null-func", 0, DIR, 0, 0},
        {"bad-name", 0, DIR + 0x200, OPD, uint64_t(-1)},
        {"too-long", 0, DIR + 0x100, OPD, uint64_t(-2)},
    };
    std::strcpy(r.name_buffer, "_BAD");
    r.put(DIR + 0x200, r.name_buffer, 5);
    for (const Case &c : cases) {
        const size_t before = r.recorder.report().published_count;
        r.call_check(c.version, c.dir, 1, c.func, 0x1234);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.execution.ppu.cpu.gpr[3] == c.expected);
        CHECK(r.execution.ppu.cpu.gpr[2] == TOC && r.get_u64(TOC_SLOT) == TOC);
        CHECK(!r.control->gamedata_check.callback_invoked);
        CHECK(!cb.invoked);
        CHECK(r.recorder.report().published_count == before + 1);
        CHECK(r.storage.creates == 0 && r.storage.writes == 0);
        r.put_u64(TOC_SLOT, TOC_SENTINEL);
    }
}

void dispatch_and_frame_failures()
{
    {
        Runtime r;
        CallbackState cb;
        r.setup_callback(cb, Variant::Ok);
        r.put32(OPD, 0x00dead00u);
        const auto before = r.snapshot();
        r.call_check();
        CHECK(r.control->stop == StartupStop::DispatchFailure);
        CHECK(!cb.invoked && r.lookup.calls == 1);
        CHECK(r.snapshot() == before);
        CHECK(r.storage.creates == 0 && r.storage.writes == 0);
    }
    {
        Runtime r(false, false);
        CallbackState cb;
        r.setup_callback(cb, Variant::Ok);
        const auto before = r.snapshot();
        r.call_check();
        CHECK(r.control->stop == StartupStop::InvalidMemory);
        CHECK(!cb.invoked);
        CHECK(r.snapshot() == before);
        CHECK(r.get_u64(TOC_SLOT) == TOC_SENTINEL);
    }
}

void storage_failure_stops()
{
    Runtime r;
    CallbackState cb;
    r.setup_callback(cb, Variant::Ok);
    r.storage.fail_free = GAMEDATA_STORAGE_IO_ERROR;
    const auto before = r.snapshot();
    r.call_check();
    CHECK(r.control->stop == StartupStop::ProviderFailure);
    CHECK(!cb.invoked);
    CHECK(r.snapshot() == before);
}

void thread_exit_gate()
{
    {
        Runtime r;
        r.execution.child_thread_exit_allowed = true;
        r.call_exit(UINT64_C(0x1234567890abcdef));
        CHECK(r.control->stop == StartupStop::GuestThreadExit);
        CHECK(r.control->has_guest_exit);
        CHECK(r.control->guest_exit_value == UINT64_C(0x1234567890abcdef));
        CHECK(r.control->guest_thread_exit_calls == 1);
        CHECK(r.get_u64(TOC_SLOT) == TOC_SENTINEL);   // no TOC publish: the thread ends here
    }
    {
        Runtime r;
        r.call_exit(7);
        CHECK(r.control->stop == StartupStop::UnsupportedSyscall);
        CHECK(!r.control->has_guest_exit && r.control->guest_exit_value == 0);
        CHECK(r.control->guest_thread_exit_calls == 1);
    }
}

void tail_call_is_drained()
{
    // decision 14: a funcStat that ends with a TAIL CALL must have that tail call run before CheckCreate2 reads the records back.
    Runtime r;
    CallbackState cb;
    r.setup_callback(cb, Variant::TailCall);
    r.call_check();
    const auto &o = r.control->gamedata_check;
    CHECK(r.control->stop == StartupStop::None);
    CHECK(cb.invoked && cb.args_ok && cb.tail_invoked);
    CHECK(g_trampoline_fn == nullptr);                       // the drain consumed the pending tail call
    CHECK(o.result == 0 && o.status == GAMEDATA_OK);
    CHECK(o.set_param == PARAM);                             // ONLY the tail call wrote it
    CHECK(o.action.action == GAMEDATA_ACTION_CREATED_OR_UPDATED);
    CHECK(r.storage.dir_exists && r.storage.usrdir_created);   // the tail-call write was observed, so the directory was created
    CHECK(r.control->gamedata_check_calls == 1 && r.control->gamedata_check_returns == 1);
    CHECK(r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[1] == STACK);
}

void size_null_pointer()
{
    Runtime r;
    r.call_size(0);
    const auto &s = r.control->gamedata_size;
    CHECK(r.control->stop == StartupStop::None);
    CHECK(r.execution.ppu.cpu.gpr[3] == 0 && r.execution.ppu.cpu.gpr[2] == TOC);
    CHECK(s.has_result && !s.written && s.toc_written && s.value == 0);
    CHECK(r.get32(SIZE_OUT) == 0xdeadbeefu);
    CHECK(r.control->gamedata_size_returns == 1);
}
}   // namespace

extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }

int main()
{
    new_data_ok_flow();
    existing_data_and_size();
    error_variants_create_nothing();
    argument_quirks();
    dispatch_and_frame_failures();
    storage_failure_stops();
    thread_exit_gate();
    tail_call_is_drained();
    size_null_pointer();
    if (failures) {
        std::printf("STARTUP_GAMEDATA FAIL failures=%d\n", failures);
        return 1;
    }
    std::printf("STARTUP_GAMEDATA PASS\n");
    return 0;
}

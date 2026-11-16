/* Focused host test for the batch-i1 startup imports that are neither SPURS nor FS-attach:
 *
 *   cellPadInit 0x1CF98800
 *   cellPadSetPortSetting   0x578E3C98
 *   cellFsStat 0x7DE6DCED (+ decision 24)
 *   cellFsOpendir           0x3F61245C
 *   cellFsReaddir           0x5C74903D
 *   cellFsClosedir          0xFF42DCC3
 *   cellFsRmdir             0x2796FDF3
 *   cellFsUnlink            0x7F4677A8
 *
 * The REAL startup_execution.cpp dispatchers, an owned StartupExecution, a coherent in-memory guest
 * and the REAL POSIX storage adapter over a SCRATCH tree inside the build directory. Nothing is
 * written to the SD card, no controller source exists, no game runs, no GPU is touched.
 *
 * Two properties this file exists to prove:
 *  1. the pad state is INSTANCE-owned: a second cellPadInit in the same control block answers
 *     0x80121103, and a pad setter before any init answers 0x80121104. A per-call state copy would
 *     make both look like the first call, which is exactly the silent failure the game hides;
 *  2. every file-system call is captured with its guest path and the mount it resolved to, because
 *     "which mount does the purge target" (B2-S5) cannot be answered any other way.
 *
 * Layout: the pad and fs types and StartupControl/StartupExecution members are UNCONDITIONAL, so
 * this file compiles with the flags off too; the flag-off cases prove the NIDs then stop
 * UnknownImport with no guest byte and no storage call. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "../adapters/guard_pthread.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("PADFS_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};

#ifdef PS3_NATIVE_STARTUP_FS
// Composition-owned storage, exactly as the composition root declares it (a file-scope object, never a
// thread stack: fs_service is about 42 KiB).
fs_storage_posix startup_fs_storage;
StartupFsContext startup_fs_context;
#endif

constexpr uint32_t WINDOW = 0x0e100000u, WINDOW_SIZE = 0x40000u;
constexpr uint32_t PATH_BUFFER = WINDOW + 0x1000u;
constexpr uint32_t OUT_BUFFER = WINDOW + 0x2000u;
constexpr uint32_t SP = WINDOW + 0x3000u;
constexpr uint32_t TOC_SLOT = SP + 0x28;
constexpr uint64_t TOC = UINT64_C(0x0078bf78);

constexpr uint32_t PAD_INIT_NID = 0x1cf98800u;
constexpr uint32_t PAD_PORT_NID = 0x578e3c98u;
constexpr uint32_t PAD_GET_INFO2_NID = 0xa703a51du;
constexpr uint32_t PAD_GET_DATA_NID = 0x8b72cda1u;
constexpr uint32_t PAD_ACT_NID = 0xf65544eeu;
constexpr uint32_t PAD_INFO2_OUT = OUT_BUFFER + 0x5000u;
constexpr uint32_t PAD_DATA_OUT = OUT_BUFFER + 0x6000u;
constexpr uint32_t FS_STAT_NID = 0x7de6dcedu;
constexpr uint32_t FS_OPENDIR_NID = 0x3f61245cu;
constexpr uint32_t FS_READDIR_NID = 0x5c74903du;
constexpr uint32_t FS_CLOSEDIR_NID = 0xff42dcc3u;
constexpr uint32_t FS_RMDIR_NID = 0x2796fdf3u;
constexpr uint32_t FS_UNLINK_NID = 0x7f4677a8u;
constexpr uint32_t FS_OPEN_NID = 0x718bf5f8u;
constexpr uint32_t FS_READ_NID = 0x4d5ff8e2u;
constexpr uint32_t FS_WRITE_NID = 0xecdcf2abu;
constexpr uint32_t FS_CLOSE_NID = 0x2cb51f0du;
constexpr uint32_t FS_FSTAT_NID = 0xef3efa34u;
constexpr uint32_t FS_LSEEK_NID = 0xa397d042u;
constexpr uint32_t FS_MKDIR_NID = 0xba901fe6u;
constexpr uint32_t FS_RENAME_NID = 0xf12eecc8u;
constexpr uint32_t FS_UTIME_NID = 0xbef554a4u;
constexpr uint32_t FS_FSYNC_NID = 0x967a162bu;

constexpr uint32_t FS_SYSCALL_OPEN = 801u;
constexpr uint32_t FS_SYSCALL_READ = 802u;
constexpr uint32_t FS_SYSCALL_WRITE = 803u;
constexpr uint32_t FS_SYSCALL_CLOSE = 804u;
constexpr uint32_t FS_SYSCALL_FSTAT = 809u;
constexpr uint32_t FS_SYSCALL_MKDIR = 811u;
constexpr uint32_t FS_SYSCALL_RENAME = 812u;
constexpr uint32_t FS_SYSCALL_UTIME = 815u;
constexpr uint32_t FS_SYSCALL_FSYNC = 820u;
constexpr uint32_t UTIME_BUFFER = OUT_BUFFER + 0x7000u;
constexpr uint32_t PATH_BUFFER_2 = WINDOW + 0x9000u;
constexpr uint32_t FS_SYSCALL_LSEEK = 818u;
constexpr uint32_t DATA_BUFFER = WINDOW + 0x8000u;

struct Guest {
    struct Region { uint32_t ea; std::vector<unsigned char> bytes; };
    std::vector<Region> regions;
    void add(uint32_t ea, size_t size, unsigned char fill)
    { regions.push_back(Region{ea, std::vector<unsigned char>(size, fill)}); }
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
        auto *r = static_cast<Guest *>(p)->find(a, size_t(n));
        if (!r) return PS3_GUEST_MEMORY_INVALID_RANGE;
        std::memcpy(r->bytes.data() + (a - r->ea), v, n);
        return PS3_GUEST_MEMORY_OK;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};

struct Fixture {
    Guest guest;
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

    Fixture()
    {
        guest.add(WINDOW, WINDOW_SIZE, 0u);
        ranges.push_back({WINDOW, WINDOW_SIZE, true});
        CHECK(!ps3_pthread_guard_init(&guard));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), ps3_pthread_guard_port(&guard),
            owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges.data(), ranges.size()));
        services.memory = &binding->coherent_memory;
        execution.control = control.get();
        execution.ppu.cpu.gpr[1] = SP;
        execution.ppu.cpu.gpr[2] = TOC;
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Suffix, 0));
        CHECK(native_startup_admit_recorded(execution, *binding, services, recorder));
    }
    ~Fixture()
    {
        if (execution.ppu.admitted) CHECK(ppu_memory_release_after_join(execution.ppu) == PS3_GUEST_MEMORY_OK);
        ps3_pthread_guard_destroy(&guard);
    }
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n)
    { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    void put32(uint32_t a, uint32_t v)
    {
        const unsigned char b[4]{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        put(a, b, 4);
    }
    void put_path(const char *text) { put(PATH_BUFFER, text, std::strlen(text) + 1u); }
    void put_path_2(const char *text) { put(PATH_BUFFER_2, text, std::strlen(text) + 1u); }
    uint64_t get(uint32_t a, size_t n)
    {
        unsigned char b[8]{};
        CHECK(mem().read(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK);
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v = v << 8 | b[i];
        return v;
    }
    // Runs one import. Returns true when it RETURNED; false when it stopped terminally.
    bool run(uint32_t nid, uint64_t r3 = 0, uint64_t r4 = 0, uint64_t r5 = 0,
        uint64_t r6 = 0, uint64_t r7 = 0, uint64_t r8 = 0)
    {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5;
        cpu.gpr[6] = r6; cpu.gpr[7] = r7; cpu.gpr[8] = r8;
        cpu.gpr[1] = SP; cpu.gpr[2] = TOC; cpu.gpr[11] = 0;
        control->stop = StartupStop::None;
        control->import_count = 0;
        active = control.get();
        control->cpu = &cpu;
        bool returned = true;
        try { ps3_hle_call(nid, &cpu); active = nullptr; }
        catch (const Terminal &) { returned = false; active = nullptr; }
        return returned;
    }
    bool run_syscall(uint64_t number, uint64_t r3 = 0, uint64_t r4 = 0, uint64_t r5 = 0,
        uint64_t r6 = 0, uint64_t r7 = 0, uint64_t r8 = 0)
    {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[1] = SP; cpu.gpr[2] = TOC; cpu.gpr[11] = number;
        cpu.gpr[3] = r3; cpu.gpr[4] = r4; cpu.gpr[5] = r5;
        cpu.gpr[6] = r6; cpu.gpr[7] = r7; cpu.gpr[8] = r8;
        control->stop = StartupStop::None;
        control->import_count = 0;
        active = control.get();
        control->cpu = &cpu;
        bool returned = true;
        try { lv2_syscall(&cpu); active = nullptr; }
        catch (const Terminal &) { returned = false; active = nullptr; }
        return returned;
    }
};
}   // namespace

extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }

namespace {
#ifdef PS3_NATIVE_STARTUP_PAD
/* ---- cellPadInit (0x1CF98800) ----------------------------------------------------------------
 * The game's own argument: r3 = 2 (func_00154194, LR 0x001541B8). Nothing else is read and no guest
 * byte is written: the whole pad module state is firmware-private. */
void pad_init_served()
{
    Fixture f;
    const auto before = f.execution.ppu.cpu;
    (void)before;
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);                 // success
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC);               // no TOC write; the recompiler restores it
    CHECK(f.control->pad_init_calls == 1 && f.control->pad_init_returns == 1);
    CHECK(f.control->pad_init.result == 0);
    CHECK(f.control->pad_init.count == 2u && f.control->pad_init.argument_checked);
    CHECK(f.control->pad_init.state.initialized == 1u && f.control->pad_init.state.max_pads == 2u);
    CHECK(f.control->pad_init.state.attached_pads == 1u);   // PAD_ASSUMED_PAD_COUNT: no controller source
    CHECK(f.control->pad_init_requests == 0);               // no non-zero result: the guest does not skip
    CHECK(!f.control->pad_init.second_call);
    CHECK(f.execution.ppu.cpu.gpr[4] == 0 && f.execution.ppu.cpu.gpr[5] == 0);
}

/* The SECOND call in the SAME control block answers 0x80121103, because the state is instance-owned.
 * A per-call state copy would return 0 here and hide the game's silent skip. */
void pad_init_second_call_is_instance_owned()
{
    Fixture f;
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.control->pad_init.result == 0);
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121103))));
    CHECK(f.control->pad_init.result == PAD_ERROR_ALREADY_INITIALIZED);
    CHECK(f.control->pad_init.second_call);
    CHECK(f.control->pad_init_calls == 2 && f.control->pad_init_returns == 2);
    CHECK(f.control->pad_init_requests == 1);               // THE signal that the game skipped its pad block
    CHECK(f.control->pad_init.state.max_pads == 2u);         // untouched by the second call
}

void pad_init_refusals()
{
    {   // count outside [1, 0x7F]: 0 and 0x80 -> 0x80121102
        for (uint32_t count : {0u, 0x80u}) {
            Fixture f;
            CHECK(f.run(PAD_INIT_NID, count));
            CHECK(f.control->stop == StartupStop::None);
            CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121102))));
            CHECK(f.control->pad_init.result == PAD_ERROR_INVALID_ARGUMENT);
            CHECK(!f.control->pad_init.argument_checked);
            CHECK(f.control->pad_init.state.initialized == 0u);
            CHECK(f.control->pad_init_requests == 1);
        }
    }
    {   // 0x7F is the INCLUSIVE bound and passes
        Fixture f;
        CHECK(f.run(PAD_INIT_NID, 0x7Fu));
        CHECK(f.execution.ppu.cpu.gpr[3] == 0 && f.control->pad_init.state.max_pads == 0x7Fu);
    }
}

/* ---- cellPadSetPortSetting (0x578E3C98) --------------------------------------------------------
 * The game calls it for ports 0 and 1 with setting 6, and ignores r3 (func_00154194's loop compares
 * the port, not the result). The record offset moves by PAD_RECORD_STRIDE per port. */
void pad_set_port_setting_served()
{
    Fixture f;
    CHECK(f.run(PAD_PORT_NID, 0u, 6u));                     // before any init -> 0x80121104
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121104))));
    CHECK(f.control->pad_set_port_setting.result == PAD_ERROR_UNINITIALIZED);
    CHECK(!f.control->pad_set_port_setting.applied);

    CHECK(f.run(PAD_INIT_NID, 2u));
    for (uint32_t port : {0u, 1u}) {
        CHECK(f.run(PAD_PORT_NID, port, 6u));
        CHECK(f.control->stop == StartupStop::None);
        CHECK(f.execution.ppu.cpu.gpr[3] == 0);
        CHECK(f.control->pad_set_port_setting.result == 0);
        CHECK(f.control->pad_set_port_setting.applied);
        CHECK(f.control->pad_set_port_setting.port_no == port);
        CHECK(f.control->pad_set_port_setting.setting == 6u);
        // setting 6 = bits 1 and 2 -> both record fields set, at PAD_RECORD_BASE + 232*port.
        CHECK(f.control->pad_set_port_setting.detail.field_a == 1u);
        CHECK(f.control->pad_set_port_setting.detail.field_b == 1u);
        CHECK(f.control->pad_set_port_setting.detail.record_offset ==
            PAD_RECORD_BASE + PAD_RECORD_STRIDE * port);
        // sc 503 is recorded, not issued: the port-open path that arms the gate is not modelled.
        CHECK(!f.control->pad_set_port_setting.syscall_replayed);
    }
    CHECK(f.control->pad_set_port_setting_calls == 3);
    CHECK(f.control->pad_set_port_setting_returns == 3);

    // port > PAD_PORT_MAX -> 0x80121102, and the record is not touched.
    CHECK(f.run(PAD_PORT_NID, uint32_t(PAD_PORT_MAX) + 1u, 6u));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121102))));
    CHECK(f.control->pad_set_port_setting.result == PAD_ERROR_INVALID_ARGUMENT);
    CHECK(!f.control->pad_set_port_setting.applied);
    // The inclusive bound passes.
    CHECK(f.run(PAD_PORT_NID, uint32_t(PAD_PORT_MAX), 6u));
    CHECK(f.execution.ppu.cpu.gpr[3] == 0 && f.control->pad_set_port_setting.applied);
}

/* Neither pad import writes a guest byte and neither publishes a trace event: the module state is
 * firmware-private, which is why the domain produces a byte image instead. */
void pad_writes_nothing()
{
    Fixture f;
    const size_t published0 = f.recorder.report().published_count;
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.run(PAD_PORT_NID, 0u, 6u));
    CHECK(f.recorder.report().published_count == published0);
    CHECK(!f.recorder.report().batch_active);
}
/* ---- the three poll entries, with NO process object (the pre-D1 path) ---------------------------
 * With StartupExecution::pad_process NULL the module state is the control's own pad_init.state and
 * there is no controller source, so info2 reports "nothing connected" and get_data is NO_DEVICE. */
void pad_readers_without_process()
{
    Fixture f;
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);

    CHECK(f.run(PAD_GET_INFO2_NID, PAD_INFO2_OUT));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    // CellPadInfo2: max_connect, now_connect, system_info, then the port words.
    CHECK(f.get(PAD_INFO2_OUT, 4) == 2u);
    CHECK(f.get(PAD_INFO2_OUT + 4u, 4) == 0u);              // now_connect: no controller source

    CHECK(f.control->fs.calls == 0u);                        // the pad arms never touch the fs counters
    CHECK(f.run(PAD_GET_DATA_NID, 0u, PAD_DATA_OUT));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121107))));
    CHECK(f.get(PAD_DATA_OUT, 4) == 0u);                     // nothing written

    // Before any init the readers answer UNINITIALIZED.
    Fixture blank;
    CHECK(blank.run(PAD_GET_INFO2_NID, PAD_INFO2_OUT));
    CHECK(blank.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121104))));
    CHECK(blank.run(PAD_GET_DATA_NID, 0u, PAD_DATA_OUT));
    CHECK(blank.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121104))));
}

/* A fake controller source: one standard pad on port 0 with CROSS held and its pressure word full. */
struct FakeController {
    pad_host_state state{};
};

pad_port_status fake_pad_count(void *, uint32_t *out)
{
    *out = 1u;
    return PAD_PORT_OK;
}

pad_port_status fake_pad_state(void *context, uint32_t port_no, pad_host_state *out)
{
    if (port_no != 0u) { out->connected = 0u; return PAD_PORT_OK; }
    *out = static_cast<FakeController *>(context)->state;
    return PAD_PORT_OK;
}

FakeController make_cross_held_controller()
{
    FakeController controller;
    controller.state.connected = 1u;
    controller.state.digital2 = PAD_CTRL_CROSS;
    controller.state.right_x = PAD_ANALOG_CENTRE;
    controller.state.right_y = PAD_ANALOG_CENTRE;
    controller.state.left_x = PAD_ANALOG_CENTRE;
    controller.state.left_y = PAD_ANALOG_CENTRE;
    return controller;
}

/* ---- with a StartupPadProcess: one state and one source for every guest thread ----------------- */
void pad_process_arms()
{
    Fixture f;
    FakeController controller = make_cross_held_controller();
    StartupPadProcess process;
    process.port.context = &controller;
    process.port.pad_count = fake_pad_count;
    process.port.read_state = fake_pad_state;
    f.execution.pad_process = &process;

    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(process.state.initialized == 1u);
    CHECK(process.state.attached_pads == 1u);
    // The existing observable keeps telling the truth about the process's state.
    CHECK(f.control->pad_init.state.initialized == 1u);
    CHECK(f.control->pad_init.state.max_pads == 2u);

    // The game's own two port settings (setting 6: press + sensor mode).
    for (uint32_t port : {0u, 1u}) {
        CHECK(f.run(PAD_PORT_NID, port, 6u));
        CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    }

    CHECK(f.run(PAD_GET_INFO2_NID, PAD_INFO2_OUT));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(PAD_INFO2_OUT, 4) == 2u);                    // max_connect: the cellPadInit argument
    CHECK(f.get(PAD_INFO2_OUT + 4u, 4) == 1u);               // now_connect: the fake pad
    CHECK(f.get(PAD_INFO2_OUT + 12u, 4) == 3u);              // port_status[0] CONNECTED|ASSIGN_CHANGES
    CHECK(f.get(PAD_INFO2_OUT + 40u, 4) == 6u);              // port_setting[0]: 12 + 7*4, word 0
    CHECK(process.info2_calls == 1u);

    CHECK(f.run(PAD_GET_DATA_NID, 0u, PAD_DATA_OUT));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(PAD_DATA_OUT, 4) == 24u);                    // CELL_PAD_LEN_CHANGE_SENSOR (setting bit 2)
    CHECK(f.get(PAD_DATA_OUT + 4u + 2u * 3u, 2) == 0x40u);   // digital2: CROSS
    CHECK(f.get(PAD_DATA_OUT + 4u + 2u * 14u, 2) == 0xFFu);  // press_cross fully pressed
    // `data_changed` counts the non-empty reports. Word 0 of a CellPadData is reserved and zero, so the
    // length lives at byte 3 of the first word.
    CHECK(process.data_calls == 1u && process.data_changed == 1u);

    // The next poll has nothing new: len 0, and the guest's buffer is replaced with that.
    CHECK(f.run(PAD_GET_DATA_NID, 0u, PAD_DATA_OUT));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(PAD_DATA_OUT, 4) == 0u);
    CHECK(process.data_calls == 2u && process.data_changed == 1u);

    // A port that is out of range / has no pad is NO_DEVICE, and nothing is written.
    CHECK(f.run(PAD_GET_DATA_NID, 1u, PAD_DATA_OUT));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121107))));
    CHECK(f.get(PAD_DATA_OUT, 4) == 0u);

    // A NULL output pointer is the firmware's INVALID_ARGUMENT, never a guest write attempt.
    CHECK(f.run(PAD_GET_INFO2_NID, 0u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121102))));
    CHECK(f.run(PAD_GET_DATA_NID, 0u, 0u));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121102))));

    // The actuator: a param pointer is reported as present, its absence is INVALID_ARGUMENT.
    CHECK(f.run(PAD_ACT_NID, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.run(PAD_ACT_NID, 0u, 0u));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121102))));
    CHECK(f.run(PAD_ACT_NID, 0u, OUT_BUFFER));               // still counted, still served
    CHECK(process.act_calls == 3u);
    CHECK(f.recorder.report().published_count == 0u);        // silent: no trace event at all
}

/* A SECOND execution borrowing the same process sees the first thread's initialised state: this is
 * the whole point of the shared object (each thread has its own StartupControl). */
void pad_process_is_shared_across_executions()
{
    Fixture first;
    StartupPadProcess process;
    first.execution.pad_process = &process;
    CHECK(first.run(PAD_INIT_NID, 2u));
    CHECK(first.execution.ppu.cpu.gpr[3] == 0);
    CHECK(process.state.initialized == 1u);

    Fixture second;
    second.execution.pad_process = &process;
    CHECK(second.run(PAD_GET_INFO2_NID, PAD_INFO2_OUT));
    CHECK(second.control->stop == StartupStop::None);
    CHECK(second.execution.ppu.cpu.gpr[3] == 0);             // NOT 0x80121104 (uninitialised)
    CHECK(second.run(PAD_INIT_NID, 2u));
    CHECK(second.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121103))));
    CHECK(process.state.max_pads == 2u);
}

/* The guard is released after every arm, including the ones that answer an error: a leaked lock
 * would wedge the next polling thread for ever. */
void pad_process_lock_is_released()
{
    Fixture f;
    StartupPadProcess process;
    f.execution.pad_process = &process;
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(f.run(PAD_PORT_NID, 0u, 6u));
    CHECK(f.run(PAD_GET_INFO2_NID, 0u));                     // NULL pointer: error path
    CHECK(f.run(PAD_GET_DATA_NID, 9u, PAD_DATA_OUT));        // port out of range: error path
    CHECK(f.run(PAD_ACT_NID, 0u, 0u));                       // NULL param: error path
    // The guard was released: the flag can be taken and cleared here.
    CHECK(!process.lock.test_and_set(std::memory_order_acquire));
    process.lock.clear(std::memory_order_release);
    CHECK(f.run(PAD_GET_INFO2_NID, PAD_INFO2_OUT));
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
}

/* A port whose three function pointers are all NULL is handed to the service as NULL: "no controller
 * source" stays one representation, so the poll entries behave exactly as they do without a process. */
void pad_process_without_controller_source()
{
    Fixture f;
    StartupPadProcess process;
    f.execution.pad_process = &process;
    CHECK(f.run(PAD_INIT_NID, 2u));
    CHECK(process.state.attached_pads == 1u);                // PAD_ASSUMED_PAD_COUNT, not the port
    CHECK(f.run(PAD_GET_INFO2_NID, PAD_INFO2_OUT));
    CHECK(f.execution.ppu.cpu.gpr[3] == 0 && f.get(PAD_INFO2_OUT + 4u, 4) == 0u);
    CHECK(f.run(PAD_GET_DATA_NID, 0u, PAD_DATA_OUT));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(int32_t(0x80121107))));
}

#endif  // PS3_NATIVE_STARTUP_PAD

#ifdef PS3_NATIVE_STARTUP_FS
/* ---- the six `sys_fs` imports over the REAL POSIX adapter and a SCRATCH tree ------------------ */

std::string scratch_root()
{
    // The tree lives under the build directory the verifier passes, never on the SD card.
    const char *dir = std::getenv("STARTUP_PAD_FS_SCRATCH");
    std::string root = dir && *dir ? std::string(dir) : std::string("build/pad-fs-scratch");
    return root;
}

std::string shell_quote(const std::string &text)
{
    std::string out = "'";
    for (char c : text) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
}

// Composes the root exactly as the composition root does: POSIX adapter, then the five canonical mounts.
void compose(bool &ok)
{
    ok = false;
    const std::string root = scratch_root();
    if (fs_storage_posix_init(&startup_fs_storage, root.c_str()) != 0) return;
    startup_fs_context = StartupFsContext{};
    startup_fs_context.storage = startup_fs_storage;
    const fs_storage_port port = fs_storage_posix_port(&startup_fs_context.storage);
    fs_service_init(&startup_fs_context.service, &port);
    for (uint32_t index = 0; index < FS_MOUNT_CAPACITY; ++index) {
        const char *guest = fs_canonical_guest_mount(index);
        if (!guest) continue;
        if (fs_mount_add(&startup_fs_context.service, guest, root.c_str()) != 0) return;
    }
    startup_fs_context.active = true;
    ok = true;
}

struct FsFixture : Fixture {
    bool composed = false;
    FsFixture()
    {
        compose(composed);
        if (composed) {
            execution.fs = &startup_fs_context;
            control->fs.active = true;
            control->fs.root_ready = true;
        }
    }
};

void fs_environment()
{
    // The scratch tree the verifier provisions: <root>/hdd0/game/NPWR00001/... and one file.
    const std::string root = scratch_root();
    const std::string hdd0 = root + "/hdd0";
    const std::string game = hdd0 + "/game";
    const std::string title = game + "/NPWR00001";
    const std::string saved = title + "/SAVEDATA";
    const std::string file = saved + "/DATA.BIN";
    const std::string cmd = "rm -rf " + shell_quote(hdd0) + " && mkdir -p " + shell_quote(saved) +
        " && : > " + shell_quote(file);
    const int rc = std::system(cmd.c_str());
    CHECK(rc == 0);
}

void fs_syscall_read_existing()
{
    FsFixture f;
    CHECK(f.composed);
    CHECK(std::system(("mkdir -p " + shell_quote(scratch_root() + "/hdd0/syscall")).c_str()) == 0);
    const std::string host = scratch_root() + "/hdd0/syscall/READ.BIN";
    FILE *stream = std::fopen(host.c_str(), "wb");
    CHECK(stream != nullptr);
    if (stream) {
        const char content[] = "abcdefghij";
        CHECK(std::fwrite(content, 1, 10, stream) == 10u);
        CHECK(std::fclose(stream) == 0);
    }
    const std::string path = "/dev_hdd0/syscall/READ.BIN";
    f.put_path(path.c_str());
    f.put32(OUT_BUFFER, 0xdeadbeefu);
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC);
    CHECK(f.get(OUT_BUFFER, 4) == 3u);
    CHECK(f.control->fs.captured == 1u);
    CHECK(f.control->fs.capture[0].sc == FS_SYSCALL_OPEN);
    CHECK(f.control->fs.capture[0].resolved && f.control->fs.capture[0].mount == 0u);
    CHECK(std::strcmp(f.control->fs.capture[0].path, path.c_str()) == 0);

    f.put32(OUT_BUFFER + 0x100u, 0xdeadbeefu);
    CHECK(f.run_syscall(FS_SYSCALL_READ, 3u, DATA_BUFFER, 5u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 5u);
    CHECK(f.get(DATA_BUFFER, 5) == UINT64_C(0x6162636465));

    CHECK(f.run_syscall(FS_SYSCALL_READ, 3u, DATA_BUFFER, 5u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 5u);
    CHECK(f.get(DATA_BUFFER, 5) == UINT64_C(0x666768696a));

    f.put32(OUT_BUFFER + 0x100u, 0xdeadbeefu);
    CHECK(f.run_syscall(FS_SYSCALL_READ, 3u, DATA_BUFFER, 5u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 0u);

    CHECK(f.run_syscall(FS_SYSCALL_LSEEK, 3u, 2u, 0u, OUT_BUFFER + 0x200u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x200u, 8) == 2u);
    CHECK(f.run_syscall(FS_SYSCALL_LSEEK, 3u, 0u, 2u, OUT_BUFFER + 0x200u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x200u, 8) == 10u);
    CHECK(f.run_syscall(FS_SYSCALL_LSEEK, 3u, 0u, 7u, OUT_BUFFER + 0x200u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EINVAL)));

    CHECK(f.run_syscall(FS_SYSCALL_FSTAT, 3u, OUT_BUFFER + 0x300u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x300u + 0x24u, 8) == 10u);

    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, 3u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, 3u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EBADF)));

    f.put_path("/dev_hdd0/syscall/ABSENT.BIN");
    f.put32(OUT_BUFFER + 0x400u, 0xdeadbeefu);
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, OUT_BUFFER + 0x400u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    CHECK(f.get(OUT_BUFFER + 0x400u, 4) == 0xdeadbeefu);

    f.put_path(path.c_str());
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, 0u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));

    CHECK(f.run_syscall(FS_SYSCALL_READ, 999u, DATA_BUFFER, 1u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EBADF)));
    CHECK(f.run_syscall(FS_SYSCALL_READ, 3u, DATA_BUFFER, 0x80000000u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EINVAL)));
    CHECK(f.run_syscall(FS_SYSCALL_READ, 3u, 0x30000000u, 1u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
}

void fs_syscall_create_write_read()
{
    FsFixture f;
    CHECK(f.composed);
    CHECK(std::system(("mkdir -p " + shell_quote(scratch_root() + "/hdd0/syscall")).c_str()) == 0);
    const std::string path = "/dev_hdd0/syscall/SYS.BIN";
    std::remove((scratch_root() + "/hdd0/syscall/SYS.BIN").c_str());
    f.put_path(path.c_str());
    f.put32(OUT_BUFFER, 0xdeadbeefu);
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0101u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t fd = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(fd == 3u);

    const char content[] = "GHIJKL";
    f.put(DATA_BUFFER, content, 6u);
    CHECK(f.run_syscall(FS_SYSCALL_WRITE, fd, DATA_BUFFER, 6u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 6u);
    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, fd));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);

    f.put_path(path.c_str());
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t reopened = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(f.run_syscall(FS_SYSCALL_READ, reopened, DATA_BUFFER, 6u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 6u);
    CHECK(f.get(DATA_BUFFER, 6) == UINT64_C(0x4748494a4b4c));
    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, reopened));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
}

void fs_import_file_io()
{
    FsFixture f;
    CHECK(f.composed);
    CHECK(std::system(("mkdir -p " + shell_quote(scratch_root() + "/hdd0/syscall")).c_str()) == 0);
    const std::string host = scratch_root() + "/hdd0/syscall/IMPORT.BIN";
    FILE *stream = std::fopen(host.c_str(), "wb");
    CHECK(stream != nullptr);
    if (stream) {
        const char content[] = "abcdefghij";
        CHECK(std::fwrite(content, 1, 10, stream) == 10u);
        CHECK(std::fclose(stream) == 0);
    }
    const std::string path = "/dev_hdd0/syscall/IMPORT.BIN";
    f.put_path(path.c_str());
    f.put32(OUT_BUFFER, 0xdeadbeefu);
    CHECK(f.run(FS_OPEN_NID, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC);
    CHECK(f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.get(OUT_BUFFER, 4) == 3u);
    CHECK(f.execution.fs->process_logged == 1u);
    CHECK(f.execution.fs->process_log[0].sc == FS_SYSCALL_OPEN);
    CHECK(f.execution.fs->process_log[0].import_form);
    CHECK(f.execution.fs->process_log[0].rc == 0);
    CHECK(std::strcmp(f.execution.fs->process_log[0].path, path.c_str()) == 0);

    CHECK(f.run(FS_READ_NID, 3u, DATA_BUFFER, 5u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 5u);
    CHECK(f.get(DATA_BUFFER, 5) == UINT64_C(0x6162636465));

    CHECK(f.run(FS_READ_NID, 3u, DATA_BUFFER, 5u, 0u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(DATA_BUFFER, 5) == UINT64_C(0x666768696a));

    CHECK(f.run(FS_LSEEK_NID, 3u, 0u, 2u, OUT_BUFFER + 0x200u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x200u, 8) == 10u);

    CHECK(f.run(FS_FSTAT_NID, 3u, OUT_BUFFER + 0x300u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x300u + 0x24u, 8) == 10u);

    CHECK(f.run(FS_CLOSE_NID, 3u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.run(FS_CLOSE_NID, 3u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EBADF)));

    f.put_path("/dev_hdd0/syscall/IMPORT-ABSENT.BIN");
    f.put32(OUT_BUFFER + 0x400u, 0xdeadbeefu);
    CHECK(f.run(FS_OPEN_NID, PATH_BUFFER, 0u, OUT_BUFFER + 0x400u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.get(OUT_BUFFER + 0x400u, 4) == 0xdeadbeefu);
    CHECK(f.execution.fs->process_logged == 2u);
    CHECK(f.execution.fs->process_log[1].sc == FS_SYSCALL_OPEN);
    CHECK(f.execution.fs->process_log[1].import_form);
    CHECK(f.execution.fs->process_log[1].rc == FS_ERROR_ENOENT);
}

void fs_import_write_null()
{
    FsFixture f;
    CHECK(f.composed);
    CHECK(std::system(("mkdir -p " + shell_quote(scratch_root() + "/hdd0/syscall")).c_str()) == 0);
    const std::string path = "/dev_hdd0/syscall/IMPORTW.BIN";
    std::remove((scratch_root() + "/hdd0/syscall/IMPORTW.BIN").c_str());
    f.put_path(path.c_str());
    CHECK(f.run(FS_OPEN_NID, PATH_BUFFER, 0101u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t fd = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(fd == 3u);

    const char content[] = "GHIJKL";
    f.put(DATA_BUFFER, content, 6u);
    CHECK(f.run(FS_WRITE_NID, fd, DATA_BUFFER, 6u, 0u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.run(FS_CLOSE_NID, fd));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);

    f.put_path(path.c_str());
    CHECK(f.run(FS_OPEN_NID, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t reopened = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(f.run(FS_READ_NID, reopened, DATA_BUFFER, 6u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 6u);
    CHECK(f.get(DATA_BUFFER, 6) == UINT64_C(0x4748494a4b4c));
    CHECK(f.run(FS_CLOSE_NID, reopened));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
}

void fs_mixed_raw_import()
{
    FsFixture f;
    CHECK(f.composed);
    CHECK(std::system(("mkdir -p " + shell_quote(scratch_root() + "/hdd0/syscall")).c_str()) == 0);
    const std::string host = scratch_root() + "/hdd0/syscall/MIXED.BIN";
    FILE *stream = std::fopen(host.c_str(), "wb");
    CHECK(stream != nullptr);
    if (stream) {
        const char content[] = "abcdefghij";
        CHECK(std::fwrite(content, 1, 10, stream) == 10u);
        CHECK(std::fclose(stream) == 0);
    }
    const std::string path = "/dev_hdd0/syscall/MIXED.BIN";
    f.put_path(path.c_str());
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t fd = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(f.run(FS_READ_NID, fd, DATA_BUFFER, 5u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get(OUT_BUFFER + 0x100u, 8) == 5u);
    CHECK(f.get(DATA_BUFFER, 5) == UINT64_C(0x6162636465));
    CHECK(f.execution.fs->process_logged == 1u);
    CHECK(f.execution.fs->process_log[0].sc == FS_SYSCALL_OPEN);
    CHECK(!f.execution.fs->process_log[0].import_form);
    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, fd));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
}

void fs_process_log_bounded()
{
    FsFixture f;
    CHECK(f.composed);
    f.put_path("/dev_hdd0");
    for (unsigned index = 0; index < STARTUP_FS_PROCESS_LOG_CAPACITY + 2u; ++index) {
        CHECK(f.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
        CHECK(f.control->stop == StartupStop::None);
        CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    }
    CHECK(f.execution.fs->process_logged == STARTUP_FS_PROCESS_LOG_CAPACITY);
    CHECK(f.execution.fs->process_dropped == 2u);
    CHECK(f.execution.fs->process_log[0].sc == 808u);
    CHECK(f.execution.fs->process_log[0].import_form);
    CHECK(std::strcmp(f.execution.fs->process_log[0].path, "/dev_hdd0") == 0);
}

void fs_stat_served()
{
    FsFixture f;
    CHECK(f.composed);
    const std::string path = "/dev_hdd0/game/NPWR00001/SAVEDATA/DATA.BIN";
    f.put_path(path.c_str());
    CHECK(f.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC);               // the TOC slot was published
    CHECK(f.get(TOC_SLOT, 8) == TOC);
    // The 52-byte CellFsStat record: mode at +0x00 carries IFREG, size at +0x24 is the file's size.
    const uint32_t mode = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK((mode & 0170000u) == 0100000u);
    CHECK(f.get(OUT_BUFFER + 0x24, 8) == 0u);
    CHECK(f.control->fs.calls == 1 && f.control->fs.returns == 1);
    CHECK(f.control->fs.last_rc == 0);
    CHECK(f.control->fs.counters.calls_stat == 1);
    // THE B2-S5 CAPTURE: the guest path and the mount it resolved to.
    CHECK(f.control->fs.captured == 1);
    CHECK(f.control->fs.capture[0].sc == 808u);
    CHECK(f.control->fs.capture[0].resolved);
    CHECK(f.control->fs.capture[0].mount == 0u);            // /dev_hdd0 is index 0
    CHECK(std::strcmp(f.control->fs.capture[0].path, path.c_str()) == 0);
    CHECK(f.control->fs.capture[0].rc == 0);
    CHECK(std::strcmp(fs_canonical_host_leaf(f.control->fs.capture[0].mount), "hdd0") == 0);
}

void fs_stat_missing_is_enoent()
{
    FsFixture f;
    CHECK(f.composed);
    f.put_path("/dev_hdd0/game/NPWR00001/SAVEDATA/ABSENT.BIN");
    CHECK(f.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    CHECK(f.control->fs.last_rc == FS_ERROR_ENOENT);
    CHECK(f.control->fs.capture[0].rc == FS_ERROR_ENOENT);
}

/* The walker's whole contract: opendir, readdir to nread == 0 (which MUST be written on the last
 * successful call), closedir. The names come back as the compact dirent with a 1/2 type tag. */
void fs_walk()
{
    FsFixture f;
    CHECK(f.composed);
    f.put_path("/dev_hdd0/game/NPWR00001");
    CHECK(f.run(FS_OPENDIR_NID, PATH_BUFFER, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t handle = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(handle != 0u);
    CHECK(f.control->fs.counters.calls_opendir == 1);
    CHECK(f.control->fs.capture[0].mount == 0u);

    std::vector<std::string> names;
    unsigned type_of_savedata = 0;
    for (unsigned guard = 0; guard < 8; ++guard) {
        // root 2026-11-11: nread sits at +0x200 because the dirent record is the real 258 bytes now (was 19)
        f.put32(OUT_BUFFER + 0x200u, 0xdeadbeefu);          // stale: nread must be REPLACED
        CHECK(f.run(FS_READDIR_NID, handle, OUT_BUFFER + 0x100u * 0u, OUT_BUFFER + 0x200u));
        CHECK(f.control->stop == StartupStop::None);
        CHECK(f.execution.ppu.cpu.gpr[3] == 0);
        const uint64_t nread = f.get(OUT_BUFFER + 0x200u, 8);
        if (nread == 0u) break;                            // end of directory: nread written as 0
        CHECK(nread == 1u);
        const unsigned char *dirent = reinterpret_cast<const unsigned char *>(
            f.guest.regions.front().bytes.data() + (OUT_BUFFER - WINDOW));
        names.emplace_back(reinterpret_cast<const char *>(dirent + 2));
        if (names.back() == "SAVEDATA") type_of_savedata = dirent[0];
        CHECK(dirent[0] == 1u || dirent[0] == 2u);          // the only two tags the walker compares
        CHECK(dirent[1] == names.back().size());
    }
    CHECK(names.size() == 1u && names[0] == "SAVEDATA");
    CHECK(type_of_savedata == FS_CELLFS_DTYPE_DIR);
    CHECK(f.control->fs.counters.calls_readdir > 0);

    CHECK(f.run(FS_CLOSEDIR_NID, handle));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.control->fs.counters.calls_closedir == 1);
    // A second close is EBADF, and it is counted.
    CHECK(f.run(FS_CLOSEDIR_NID, handle));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EBADF)));
    CHECK(f.control->fs.counters.refusals_stale_handle == 1);
    // opendir + 2 readdir (the entry, then nread == 0) + 2 closedir, all captured.
    CHECK(f.control->fs.captured == 5u);
    CHECK(f.control->fs.capture[0].sc == 805u && f.control->fs.capture[0].resolved);
    CHECK(f.control->fs.capture[1].sc == 806u && f.control->fs.capture[1].resolved == false);
}

/* Decision 24: both delete verbs really delete, confined to the root, and never return EROFS. */
void fs_deletes()
{
    FsFixture f;
    CHECK(f.composed);
    const std::string file = scratch_root() + "/hdd0/game/NPWR00001/SAVEDATA/DATA.BIN";
    f.put_path("/dev_hdd0/game/NPWR00001/SAVEDATA/DATA.BIN");
    CHECK(f.run(FS_UNLINK_NID, PATH_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.control->fs.counters.calls_unlink == 1 && f.control->fs.counters.deletes_file_ok == 1);
    CHECK(f.control->fs.capture[0].sc == 814u && f.control->fs.capture[0].mount == 0u);
    CHECK(std::system(("test ! -e " + shell_quote(file)).c_str()) == 0);   // really gone
    // Removing it again is ENOENT and never EROFS.
    CHECK(f.run(FS_UNLINK_NID, PATH_BUFFER));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    CHECK(f.execution.ppu.cpu.gpr[3] != uint64_t(int64_t(FS_ERROR_EROFS)));

    // A non-empty directory is ENOTSUP; an EMPTY one is removed for real.
    const std::string savedata = "/dev_hdd0/game/NPWR00001/SAVEDATA";
    CHECK(std::system((": > " + shell_quote(scratch_root() + "/hdd0/game/NPWR00001/SAVEDATA/X.BIN")).c_str()) == 0);
    f.put_path(savedata.c_str());
    CHECK(f.run(FS_RMDIR_NID, PATH_BUFFER));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOTSUP)));
    CHECK(std::system(("rm -f " + shell_quote(scratch_root() + "/hdd0/game/NPWR00001/SAVEDATA/X.BIN")).c_str()) == 0);
    CHECK(f.run(FS_RMDIR_NID, PATH_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.control->fs.counters.deletes_dir_ok == 1);
    CHECK(f.run(FS_RMDIR_NID, PATH_BUFFER));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));

    // The mount root itself is EACCES: a mount is not removable.
    f.put_path("/dev_hdd0");
    CHECK(f.run(FS_RMDIR_NID, PATH_BUFFER));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EACCES)));
    // And nothing escaped the root: a ".." path is clamped at the mount root.
    f.put_path("/dev_hdd0/../../../escape");
    CHECK(f.run(FS_UNLINK_NID, PATH_BUFFER));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    CHECK(f.control->fs.capture[f.control->fs.captured - 1u].mount == 0u);
}

/* Every refusal is decided BEFORE the storage call, and the capture records it. */
void fs_refusals()
{
    FsFixture f;
    CHECK(f.composed);
    {   // a path pointer outside every range -> EFAULT
        const uint32_t calls0 = uint32_t(f.control->fs.counters.calls_stat);
        CHECK(f.run(FS_STAT_NID, 0x30000000u, OUT_BUFFER));
        CHECK(f.control->stop == StartupStop::None);
        CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
        CHECK(f.control->fs.counters.calls_stat == calls0);
        CHECK(f.control->fs.capture[0].rc == FS_ERROR_EFAULT && !f.control->fs.capture[0].resolved);
    }
    {   // a NULL stat output -> EFAULT, and no storage call
        f.put_path("/dev/hdd0/nothing");
        const uint32_t calls0 = uint32_t(f.control->fs.counters.calls_stat);
        CHECK(f.run(FS_STAT_NID, PATH_BUFFER, 0u));
        CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
        CHECK(f.control->fs.counters.calls_stat == calls0);
    }
    {   // an unconfigured mount -> ENOENT, capture shows it did not resolve
        f.put_path("/dev_hdd9/x");
        CHECK(f.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
        CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
        CHECK(!f.control->fs.capture[f.control->fs.captured - 1u].resolved);
    }
    {   // readdir with a NULL dirent output -> EFAULT (the firmware's own guard, before any lookup)
        CHECK(f.run(FS_READDIR_NID, 1u, 0u, OUT_BUFFER));
        CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
        // A non-printable path byte is escaped in the capture, never printed raw.
        f.put_path("/dev_hdd0/\\x01");
        CHECK(f.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
        const std::string captured = f.control->fs.capture[f.control->fs.captured - 1u].path;
        CHECK(captured.find("\\x01") != std::string::npos);
    }
    {   // missing services: no composed context -> MissingServices, before any effect
        Fixture v;
        CHECK(!v.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
        CHECK(v.control->stop == StartupStop::MissingServices);
    }
}

/* The capture is bounded: the first 32 calls are retained, the counters are never truncated. */
void fs_capture_is_bounded()
{
    FsFixture f;
    CHECK(f.composed);
    f.put_path("/dev_hdd0");
    for (unsigned index = 0; index < STARTUP_FS_CAPTURE_CAPACITY + 4u; ++index) {
        CHECK(f.run(FS_STAT_NID, PATH_BUFFER, OUT_BUFFER));
        CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    }
    CHECK(f.control->fs.captured == STARTUP_FS_CAPTURE_CAPACITY);
    CHECK(f.control->fs.dropped == 4u);
    CHECK(f.control->fs.calls == STARTUP_FS_CAPTURE_CAPACITY + 4u);
    CHECK(f.control->fs.counters.calls_stat == STARTUP_FS_CAPTURE_CAPACITY + 4u);
}
/* ---- D1: mkdir (811), rename (812), utime (815), fsync (820), both dispatch forms ---------------
 * The game's own libc issues these as raw syscalls, exactly like open/read/close, so the raw number
 * and the import NID reach the SAME file dispatcher. Each case drives the raw form and then the same
 * request through the import NID, where the import form additionally publishes its TOC slot. */
void fs_mkdir_both_forms()
{
    FsFixture f;
    CHECK(f.composed);
    const std::string dir = scratch_root() + "/hdd0/game/NPWR00001/NEWDIR";
    CHECK(std::system(("rm -rf " + shell_quote(dir) + " && mkdir -p " +
        shell_quote(scratch_root() + "/hdd0/game/NPWR00001")).c_str()) == 0);

    f.put_path("/dev_hdd0/game/NPWR00001/NEWDIR");
    CHECK(f.run_syscall(FS_SYSCALL_MKDIR, PATH_BUFFER, 0700u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC);                // a raw syscall never touches r2
    CHECK(std::system(("test -d " + shell_quote(dir)).c_str()) == 0);
    CHECK(f.execution.fs->process_logged == 1u);
    CHECK(f.execution.fs->process_log[0].sc == FS_SYSCALL_MKDIR);
    CHECK(!f.execution.fs->process_log[0].import_form);
    CHECK(std::strcmp(f.execution.fs->process_log[0].path, "/dev_hdd0/game/NPWR00001/NEWDIR") == 0);

    // The same directory again is EEXIST, from both forms.
    CHECK(f.run_syscall(FS_SYSCALL_MKDIR, PATH_BUFFER, 0700u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EEXIST)));
    f.put_path("/dev_hdd0/game/NPWR00001/IMPORTDIR");
    CHECK(std::system(("rm -rf " + shell_quote(scratch_root() + "/hdd0/game/NPWR00001/IMPORTDIR")).c_str()) == 0);
    CHECK(f.run(FS_MKDIR_NID, PATH_BUFFER, 0700u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(std::system(("test -d " + shell_quote(scratch_root() +
        "/hdd0/game/NPWR00001/IMPORTDIR")).c_str()) == 0);
    CHECK(f.execution.fs->process_logged == 3u);
    CHECK(f.execution.fs->process_log[2].sc == FS_SYSCALL_MKDIR);
    CHECK(f.execution.fs->process_log[2].import_form);

    // The disc is read-only: /dev_bdvd is EROFS before the port is called.
    f.put_path("/dev_bdvd/HACKDIR");
    const uint64_t before = f.control->fs.counters.calls_mkdir;
    CHECK(f.run_syscall(FS_SYSCALL_MKDIR, PATH_BUFFER, 0700u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EROFS)));
    CHECK(f.control->fs.counters.calls_mkdir == before + 1u);  // counted, refused by the service
    // An unreadable path pointer is EFAULT, exactly as the other path-bearing calls answer it.
    CHECK(f.run_syscall(FS_SYSCALL_MKDIR, 0x30000000u, 0700u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
}

void fs_rename_both_forms()
{
    FsFixture f;
    CHECK(f.composed);
    const std::string dir = scratch_root() + "/hdd0/game/NPWR00001";
    const std::string from = dir + "/OLD.BIN";
    const std::string to = dir + "/NEW.BIN";
    const std::string to_guest = "/dev_hdd0/game/NPWR00001/NEW.BIN";
    CHECK(std::system(("mkdir -p " + shell_quote(dir) + " && rm -f " + shell_quote(from) + " " +
        shell_quote(to) + " && : > " + shell_quote(from)).c_str()) == 0);

    f.put_path("/dev_hdd0/game/NPWR00001/OLD.BIN");
    f.put_path_2("/dev_hdd0/game/NPWR00001/NEW.BIN");
    CHECK(f.run_syscall(FS_SYSCALL_RENAME, PATH_BUFFER, PATH_BUFFER_2));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(std::system(("test -e " + shell_quote(to) + " && test ! -e " + shell_quote(from)).c_str()) == 0);
    // The process log names the SOURCE path.
    CHECK(f.execution.fs->process_logged == 1u);
    CHECK(f.execution.fs->process_log[0].sc == FS_SYSCALL_RENAME);
    CHECK(std::strcmp(f.execution.fs->process_log[0].path, "/dev_hdd0/game/NPWR00001/OLD.BIN") == 0);

    // The file is openable under its new name through the guest too.
    f.put_path(to_guest.c_str());
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t fd = uint32_t(f.get(OUT_BUFFER, 4));
    CHECK(f.execution.fs->process_logged == 2u);   // the rename, then the successful open
    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, fd));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.fs->process_logged == 2u);   // close is not a path-bearing log entry

    // A missing source is ENOENT, from the import form as well (the open above left a live fd on NEW.BIN,
    // so the target must be a name nothing is using).
    f.put_path("/dev_hdd0/game/NPWR00001/ABSENT.BIN");
    f.put_path_2("/dev_hdd0/game/NPWR00001/RENAMED-AGAIN.BIN");
    CHECK(f.run(FS_RENAME_NID, PATH_BUFFER, PATH_BUFFER_2));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.execution.fs->process_logged == 3u);
    CHECK(f.execution.fs->process_log[2].sc == FS_SYSCALL_RENAME);
    CHECK(f.execution.fs->process_log[2].import_form);

    // An unreadable destination pointer is EFAULT before any resolution.
    const uint64_t before = f.control->fs.counters.calls_rename;
    CHECK(f.run_syscall(FS_SYSCALL_RENAME, PATH_BUFFER, 0x30000000u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
    CHECK(f.control->fs.counters.calls_rename == before);
}

void fs_fsync_served()
{
    FsFixture f;
    CHECK(f.composed);
    const std::string dir = scratch_root() + "/hdd0/syscall";
    CHECK(std::system(("mkdir -p " + shell_quote(dir)).c_str()) == 0);
    const std::string path = "/dev_hdd0/syscall/FSYNC.BIN";
    std::remove((dir + "/FSYNC.BIN").c_str());
    f.put_path(path.c_str());
    CHECK(f.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0101u, OUT_BUFFER));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    const uint32_t fd = uint32_t(f.get(OUT_BUFFER, 4));
    const char content[] = "MNOPQR";
    f.put(DATA_BUFFER, content, 6u);
    CHECK(f.run_syscall(FS_SYSCALL_WRITE, fd, DATA_BUFFER, 6u, OUT_BUFFER + 0x100u));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);

    CHECK(f.run_syscall(FS_SYSCALL_FSYNC, fd));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.control->fs.counters.calls_fsync == 1u);
    CHECK(f.run(FS_FSYNC_NID, fd));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.control->fs.counters.calls_fsync == 2u);

    CHECK(f.run_syscall(FS_SYSCALL_CLOSE, fd));
    CHECK(f.control->stop == StartupStop::None && f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.run_syscall(FS_SYSCALL_FSYNC, fd));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EBADF)));
    CHECK(f.run(FS_FSYNC_NID, 999u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EBADF)));
}

void fs_utime_served()
{
    FsFixture f;
    CHECK(f.composed);
    const std::string path = "/dev_hdd0/game/NPWR00001/SAVEDATA/DATA.BIN";
    CHECK(std::system(("mkdir -p " + shell_quote(scratch_root() + "/hdd0/game/NPWR00001/SAVEDATA") +
        " && : > " + shell_quote(scratch_root() + "/hdd0/game/NPWR00001/SAVEDATA/DATA.BIN")).c_str()) == 0);
    const uint64_t actime = UINT64_C(0x5f5e1000), modtime = UINT64_C(0x5f5e2000);
    f.put32(UTIME_BUFFER, uint32_t(actime >> 32));
    f.put32(UTIME_BUFFER + 4u, uint32_t(actime));
    f.put32(UTIME_BUFFER + 8u, uint32_t(modtime >> 32));
    f.put32(UTIME_BUFFER + 12u, uint32_t(modtime));

    f.put_path(path.c_str());
    CHECK(f.run_syscall(FS_SYSCALL_UTIME, PATH_BUFFER, UTIME_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.control->fs.counters.calls_utime == 1u);
    CHECK(f.execution.fs->process_logged == 1u);
    CHECK(f.execution.fs->process_log[0].sc == FS_SYSCALL_UTIME);

    // A NULL CellFsUtimbuf* is EFAULT, and the service is never reached.
    CHECK(f.run_syscall(FS_SYSCALL_UTIME, PATH_BUFFER, 0u));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
    CHECK(f.control->fs.counters.calls_utime == 1u);
    // So is a buffer that is not wholly readable.
    CHECK(f.run_syscall(FS_SYSCALL_UTIME, PATH_BUFFER, 0x30000000u));
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EFAULT)));
    CHECK(f.control->fs.counters.calls_utime == 1u);

    // The import form reaches the same dispatcher and publishes its TOC slot.
    CHECK(f.run(FS_UTIME_NID, PATH_BUFFER, UTIME_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.execution.ppu.cpu.gpr[2] == TOC && f.get(TOC_SLOT, 8) == TOC);
    CHECK(f.control->fs.counters.calls_utime == 2u);
    // Four path-bearing calls before this one: the served call, the NULL buffer, the unreadable buffer,
    // and this import form. Every one of them is logged, whichever way it was answered.
    CHECK(f.execution.fs->process_logged == 4u);
    CHECK(f.execution.fs->process_log[3].sc == FS_SYSCALL_UTIME);
    CHECK(f.execution.fs->process_log[3].import_form);

    // A path that does not exist is ENOENT; the disc is EROFS.
    f.put_path("/dev_hdd0/game/NPWR00001/SAVEDATA/ABSENT.BIN");
    CHECK(f.run_syscall(FS_SYSCALL_UTIME, PATH_BUFFER, UTIME_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_ENOENT)));
    f.put_path("/dev_bdvd/PS3_GAME/PARAM.SFO");
    CHECK(f.run_syscall(FS_SYSCALL_UTIME, PATH_BUFFER, UTIME_BUFFER));
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.execution.ppu.cpu.gpr[3] == uint64_t(int64_t(FS_ERROR_EROFS)));
}

#endif  // PS3_NATIVE_STARTUP_FS

constexpr int defined_pad()
{
#ifdef PS3_NATIVE_STARTUP_PAD
    return 1;
#else
    return 0;
#endif
}
constexpr int defined_fs()
{
#ifdef PS3_NATIVE_STARTUP_FS
    return 1;
#else
    return 0;
#endif
}

/* ---- feature-off: every new NID stops UnknownImport, with no guest byte and no storage call ------ */
void flag_off_is_unknown_import()
{
#if defined(PS3_NATIVE_STARTUP_PAD) || defined(PS3_NATIVE_STARTUP_FS)
    // Built WITH at least one flag: the served cases above are the proof, and this control does not apply.
    std::printf("PADFS_NEGATIVE skipped=served_build\n");
    return;
#endif
    // The eight NIDs the original control covers keep their own line (a verifier counts them exactly);
    // the seven D1 additions are reported under their own tag so neither group can hide the other.
    const uint32_t nids[] = {PAD_INIT_NID, PAD_PORT_NID, FS_STAT_NID, FS_OPENDIR_NID, FS_READDIR_NID,
        FS_CLOSEDIR_NID, FS_RMDIR_NID, FS_UNLINK_NID};
    for (uint32_t nid : nids) {
        Fixture f;
        const uint64_t before = f.get(WINDOW + 0x1000u, 8);
        const bool returned = f.run(nid, PATH_BUFFER, OUT_BUFFER, OUT_BUFFER);
        (void)returned;
        const bool stopped = f.control->stop == StartupStop::UnknownImport;
        const bool recorded = f.control->import_count == 1u && f.control->imports[0] == nid;
        const bool untouched = f.get(WINDOW + 0x1000u, 8) == before;
        std::printf("PADFS_NEGATIVE nid=0x%08x stop=%u expected_stop=%u untouched=%d imports_recorded=%d\n",
            nid, unsigned(f.control->stop), unsigned(StartupStop::UnknownImport), int(untouched),
            int(recorded));
        CHECK(stopped && recorded && untouched);
    }
    const uint32_t added_nids[] = {PAD_GET_INFO2_NID, PAD_GET_DATA_NID, PAD_ACT_NID, FS_MKDIR_NID,
        FS_RENAME_NID, FS_UTIME_NID, FS_FSYNC_NID};
    for (uint32_t nid : added_nids) {
        Fixture f;
        const uint64_t before = f.get(WINDOW + 0x1000u, 8);
        const bool returned = f.run(nid, PATH_BUFFER, OUT_BUFFER, OUT_BUFFER);
        (void)returned;
        const bool stopped = f.control->stop == StartupStop::UnknownImport;
        const bool recorded = f.control->import_count == 1u && f.control->imports[0] == nid;
        const bool untouched = f.get(WINDOW + 0x1000u, 8) == before;
        std::printf("PADFS_ADDED_NEGATIVE nid=0x%08x stop=%u expected_stop=%u untouched=%d imports_recorded=%d\n",
            nid, unsigned(f.control->stop), unsigned(StartupStop::UnknownImport), int(untouched),
            int(recorded));
        CHECK(stopped && recorded && untouched);
    }
    const uint32_t import_nids[] = {FS_OPEN_NID, FS_READ_NID, FS_WRITE_NID, FS_CLOSE_NID,
        FS_FSTAT_NID, FS_LSEEK_NID, FS_MKDIR_NID, FS_RENAME_NID, FS_UTIME_NID, FS_FSYNC_NID};
    for (uint32_t nid : import_nids) {
        Fixture f;
        const uint64_t before = f.get(WINDOW + 0x1000u, 8);
        const bool returned = f.run(nid, PATH_BUFFER, OUT_BUFFER, OUT_BUFFER);
        (void)returned;
        const bool stopped = f.control->stop == StartupStop::UnknownImport;
        const bool recorded = f.control->import_count == 1u && f.control->imports[0] == nid;
        const bool untouched = f.get(WINDOW + 0x1000u, 8) == before;
        std::printf("PADFS_IMPORT_NEGATIVE nid=0x%08x stop=%u expected_stop=%u untouched=%d imports_recorded=%d\n",
            nid, unsigned(f.control->stop), unsigned(StartupStop::UnknownImport), int(untouched),
            int(recorded));
        CHECK(stopped && recorded && untouched);
    }
    Fixture reference;
    CHECK(!reference.run_syscall(FS_SYSCALL_READ, 0u, 0u, 0u, 0u));
    const StartupStop unserved = reference.control->stop;
    Fixture raw;
    CHECK(!raw.run_syscall(FS_SYSCALL_OPEN, PATH_BUFFER, 0u, OUT_BUFFER));
    CHECK(raw.control->stop == unserved);
}
}   // namespace

int main()
{
#ifdef PS3_NATIVE_STARTUP_PAD
    pad_init_served();
    pad_init_second_call_is_instance_owned();
    pad_init_refusals();
    pad_set_port_setting_served();
    pad_writes_nothing();
    pad_readers_without_process();
    pad_process_arms();
    pad_process_is_shared_across_executions();
    pad_process_lock_is_released();
    pad_process_without_controller_source();
#endif
#ifdef PS3_NATIVE_STARTUP_FS
    fs_environment();
    fs_syscall_read_existing();
    fs_syscall_create_write_read();
    fs_import_file_io();
    fs_import_write_null();
    fs_mixed_raw_import();
    fs_process_log_bounded();
    fs_stat_served();
    fs_stat_missing_is_enoent();
    fs_walk();
    fs_deletes();
    fs_refusals();
    fs_capture_is_bounded();
    fs_mkdir_both_forms();
    fs_rename_both_forms();
    fs_fsync_served();
    fs_utime_served();
#endif
    flag_off_is_unknown_import();
    if (failures) {
        std::printf("STARTUP_PAD_FS FAIL failures=%d\n", failures);
        return 1;
    }
    std::printf("STARTUP_PAD_FS PASS pad=%d fs=%d\n",
        int(!!defined_pad()), int(!!defined_fs()));
    return 0;
}

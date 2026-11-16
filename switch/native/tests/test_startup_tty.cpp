/* Focused host test for native lv2 syscall 403 (sys_tty_write), the console sink the game's libc
 * write() (func_00452868) reaches: r3 = channel (fd 1 -> 0, fd 2 -> 1), r4 = buf, r5 = len,
 * r6 = a u32 written count; r3 = 0 on success, otherwise the wrapper sets errno and returns -1.
 * The REAL startup_execution.cpp bridge, an owned StartupExecution and a coherent in-memory guest
 * are exercised through lv2_syscall directly. Fixture only: no game, GPU, SD or network.
 * ASan proves the bounded copy never writes outside the composition-owned ring. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "../adapters/guard_pthread.h"
}
#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <vector>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("TTY_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
// Terminal-notification probes: stop()/threadExit() must close the owned trace batch and leave the
// guard unlocked before the terminal exit. Declared here exactly as the other bridge host fixtures do.
PpuMemoryTraceRecorder *terminal_recorder;
pthread_mutex_t *terminal_guard;
bool terminal_closed, terminal_unlocked;

constexpr uint32_t WINDOW = 0x0e100000u, WINDOW_SIZE = 0x40000u;
// The read-only ROM/rodata partition the game's ELF .rodata strings live in: same guest bytes, no write permission.
constexpr uint32_t RDATA = 0x0e200000u, RDATA_SIZE = 0x10000u;
constexpr uint32_t BUFFER = WINDOW + 0x1000u;
constexpr uint32_t RDATA_BUFFER = RDATA + 0x800u;
constexpr uint32_t COUNT_OUT = WINDOW + 0x2000u;
constexpr uint32_t RDATA_COUNT_OUT = RDATA + 0x1800u;
constexpr uint32_t SYS_TTY_WRITE = 403;

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
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(256);
    PpuMemoryTraceRecorder recorder;
    Ps3PpuExecutionServices services{};
    StartupTtyRing ring{};

    Fixture()
    {
        guest.add(WINDOW, WINDOW_SIZE, 0u);
        guest.add(RDATA, RDATA_SIZE, 0u);
        ranges.push_back({WINDOW, WINDOW_SIZE, true});
        ranges.push_back({RDATA, RDATA_SIZE, false});
        CHECK(!ps3_pthread_guard_init(&guard));
        CHECK(ps3_atomic_domain_init(&domain, guest.port(), ps3_pthread_guard_port(&guard),
            owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges.data(), ranges.size()));
        services.memory = &binding->coherent_memory;
        execution.control = control.get();
        execution.ppu.cpu.gpr[1] = WINDOW + 0x3000u;
        execution.ppu.cpu.gpr[2] = 0x0078bf78u;
        execution.ppu.cpu.gpr[11] = SYS_TTY_WRITE;
        execution.ppu.cpu.gpr[3] = 1;          // fd 1 -> channel 0 is the caller's mapping; the syscall sees the channel
        execution.ppu.cpu.gpr[4] = BUFFER;
        execution.ppu.cpu.gpr[5] = 0;
        execution.ppu.cpu.gpr[6] = COUNT_OUT;
        execution.tty = &ring;
        CHECK(recorder.configure(events.data(), events.size(), PpuMemoryTraceRetention::Suffix, 0));
        CHECK(native_startup_admit_recorded(execution, *binding, services, recorder));
        put32(COUNT_OUT, 0xdeadbeefu);
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
    uint32_t get32(uint32_t a)
    {
        unsigned char b[4]{};
        CHECK(mem().read(mem().context, a, b, 4) == PS3_GUEST_MEMORY_OK);
        return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    }
    // Returns true when the call RETURNED (r3 set); false when it stopped terminally.
    bool call()
    {
        terminal_recorder = &recorder; terminal_guard = &guard.mutex;
        terminal_closed = terminal_unlocked = false;
        active = control.get();
        bool returned = true;
        try { lv2_syscall(&execution.ppu.cpu); active = nullptr; }
        catch (const Terminal &) { returned = false; CHECK(terminal_closed && terminal_unlocked); }
        terminal_recorder = nullptr; terminal_guard = nullptr;
        return returned;
    }
};

#ifdef PS3_NATIVE_STARTUP_TTY
void hello_newline(uint32_t channel)
{
    Fixture f;
    const char text[] = "hello\n";
    f.put(BUFFER, text, sizeof(text) - 1);
    f.execution.ppu.cpu.gpr[3] = channel;
    f.execution.ppu.cpu.gpr[5] = sizeof(text) - 1;
    CHECK(f.call());
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get32(COUNT_OUT) == sizeof(text) - 1);          // *pwritelen = len, big-endian
    CHECK(f.ring.used == sizeof(text) - 1 && std::memcmp(f.ring.bytes, text, sizeof(text) - 1) == 0);
    CHECK(f.ring.dropped == 0);
    CHECK(f.control->tty_write_calls == 1 && f.control->tty_write_returns == 1);
    CHECK(f.control->tty_bytes == sizeof(text) - 1);
    CHECK(f.control->tty_last_channel == channel && f.control->tty_last_len == sizeof(text) - 1);
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.recorder.report().published_count == 1 && !f.recorder.report().batch_active);
}

void zero_length_reads_nothing()
{
    Fixture f;
    // A buffer pointer that is not readable at all: len 0 must not touch it.
    f.execution.ppu.cpu.gpr[4] = 0;                 // null buf
    f.execution.ppu.cpu.gpr[4] = 0x11000;           // outside every range
    f.execution.ppu.cpu.gpr[5] = 0;
    CHECK(f.call());
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get32(COUNT_OUT) == 0);
    CHECK(f.ring.used == 0 && f.ring.dropped == 0);
    CHECK(f.control->tty_write_calls == 1 && f.control->tty_write_returns == 1 && f.control->tty_bytes == 0);
    CHECK(f.recorder.report().published_count == 1);
}

void channel_out_of_range()
{
    Fixture f;
    f.execution.ppu.cpu.gpr[3] = 16;
    const auto before = f.execution.ppu.cpu;
    CHECK(!f.call());
    CHECK(f.control->stop == StartupStop::UnsupportedSyscall);
    CHECK(f.control->tty_write_calls == 1 && f.control->tty_write_returns == 0);
    CHECK(f.control->tty_last_channel == 16);
    CHECK(f.ring.used == 0 && f.ring.dropped == 0);
    CHECK(f.get32(COUNT_OUT) == 0xdeadbeefu);      // nothing written
    CHECK(f.recorder.report().published_count == 0 && !f.recorder.report().batch_active);
    CHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
}

void bad_pointers()
{
    {   // buf outside every range
        Fixture f;
        const char text[] = "hi";
        f.put(BUFFER, text, 2);
        f.execution.ppu.cpu.gpr[4] = 0x11000;
        f.execution.ppu.cpu.gpr[5] = 2;
        const auto before = f.execution.ppu.cpu;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0 && f.get32(COUNT_OUT) == 0xdeadbeefu);
        CHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
    }
    {   // buf crosses the end of the only range
        Fixture f;
        f.execution.ppu.cpu.gpr[4] = WINDOW + WINDOW_SIZE - 1;
        f.execution.ppu.cpu.gpr[5] = 4;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0);
    }
    {   // null count pointer
        Fixture f;
        const char text[] = "hi";
        f.put(BUFFER, text, 2);
        f.execution.ppu.cpu.gpr[5] = 2;
        f.execution.ppu.cpu.gpr[6] = 0;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0);
    }
    {   // count pointer outside every range
        Fixture f;
        const char text[] = "hi";
        f.put(BUFFER, text, 2);
        f.execution.ppu.cpu.gpr[5] = 2;
        f.execution.ppu.cpu.gpr[6] = 0x11000;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0);
    }
    {   // count pointer read-only: not writable, so InvalidMemory before any ring byte
        Fixture f;
        const char text[] = "hi";
        f.put(BUFFER, text, 2);
        f.execution.ppu.cpu.gpr[5] = 2;
        f.ranges[0].writable = false;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0);
    }
}

// lv2 accepts a read-only source. Before this fix the buffer was validated with the
// writable() predicate, so a console line living in .rodata stopped with InvalidMemory.
void readonly_source_is_readable()
{
    Fixture f;
    const char text[] = "rodata line\n";
    f.put(RDATA_BUFFER, text, sizeof(text) - 1);
    f.execution.ppu.cpu.gpr[3] = 0;          // channel 0, the game's stdout path
    f.execution.ppu.cpu.gpr[4] = RDATA_BUFFER;
    f.execution.ppu.cpu.gpr[5] = sizeof(text) - 1;
    CHECK(f.call());
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get32(COUNT_OUT) == sizeof(text) - 1);        // *pwritelen = len, big-endian
    CHECK(f.ring.used == sizeof(text) - 1 && std::memcmp(f.ring.bytes, text, sizeof(text) - 1) == 0);
    CHECK(f.ring.dropped == 0);
    CHECK(f.control->tty_write_calls == 1 && f.control->tty_write_returns == 1);
    CHECK(f.control->tty_bytes == sizeof(text) - 1);
    CHECK(f.control->tty_last_channel == 0 && f.control->tty_last_len == sizeof(text) - 1);
    CHECK(f.control->stop == StartupStop::None);
    CHECK(f.recorder.report().published_count == 1 && !f.recorder.report().batch_active);
}

// The written count is an OUTPUT: it stays writable-only even though the source is readable-only.
void readonly_count_pointer_stops()
{
    Fixture f;
    const char text[] = "hi";
    f.put(RDATA_BUFFER, text, 2);
    f.execution.ppu.cpu.gpr[4] = RDATA_BUFFER;
    f.execution.ppu.cpu.gpr[5] = 2;
    f.execution.ppu.cpu.gpr[6] = RDATA_COUNT_OUT;         // inside the read-only range
    const auto before = f.execution.ppu.cpu;
    CHECK(!f.call());
    CHECK(f.control->stop == StartupStop::InvalidMemory);
    CHECK(f.control->tty_write_calls == 1 && f.control->tty_write_returns == 0);
    CHECK(f.ring.used == 0 && f.ring.dropped == 0);
    CHECK(f.recorder.report().published_count == 0 && !f.recorder.report().batch_active);
    CHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
}

// A readable source still has to be bound memory: outside every range is InvalidMemory.
void source_outside_every_range_stops()
{
    {   // between the two ranges
        Fixture f;
        f.execution.ppu.cpu.gpr[4] = 0x0e1f0000u;
        f.execution.ppu.cpu.gpr[5] = 4;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0 && f.get32(COUNT_OUT) == 0xdeadbeefu);
    }
    {   // past the read-only range's end
        Fixture f;
        f.execution.ppu.cpu.gpr[4] = RDATA + RDATA_SIZE - 2;
        f.execution.ppu.cpu.gpr[5] = 4;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0);
    }
    {   // below every range
        Fixture f;
        f.execution.ppu.cpu.gpr[4] = 0x1000u;
        f.execution.ppu.cpu.gpr[5] = 1;
        CHECK(!f.call());
        CHECK(f.control->stop == StartupStop::InvalidMemory);
        CHECK(f.ring.used == 0);
    }
}

void null_ring()
{
    Fixture f;
    const char text[] = "hi";
    f.put(BUFFER, text, 2);
    f.execution.ppu.cpu.gpr[5] = 2;
    f.execution.tty = nullptr;
    const auto before = f.execution.ppu.cpu;
    CHECK(!f.call());
    CHECK(f.control->stop == StartupStop::MissingServices);
    CHECK(f.control->tty_write_calls == 1 && f.control->tty_write_returns == 0);
    CHECK(f.get32(COUNT_OUT) == 0xdeadbeefu);
    CHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
}

void overflow_is_bounded()
{
    Fixture f;
    // 8192 printable bytes: twice the ring. All must be accepted by the guest, half the bytes dropped,
    // and ASan proves nothing was written past STARTUP_TTY_RING_BYTES.
    constexpr uint32_t length = 8192;
    std::vector<unsigned char> payload(length, 'a');
    f.put(BUFFER, payload.data(), payload.size());
    f.execution.ppu.cpu.gpr[5] = length;
    CHECK(f.call());
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    CHECK(f.get32(COUNT_OUT) == length);
    CHECK(f.ring.used == STARTUP_TTY_RING_BYTES);
    CHECK(f.ring.dropped == length - STARTUP_TTY_RING_BYTES);
    CHECK(f.ring.used + f.ring.dropped == length);
    CHECK(f.control->tty_bytes == length);
    for (size_t index = 0; index < f.ring.used; ++index) CHECK(f.ring.bytes[index] == 'a');
    CHECK(f.ring.bytes[STARTUP_TTY_RING_BYTES - 1] == 'a');
}

void escaping()
{
    Fixture f;
    const unsigned char payload[6]{'A', 0x00, 0x7f, '\t', '\n', 'Z'};
    f.put(BUFFER, payload, sizeof(payload));
    f.execution.ppu.cpu.gpr[5] = sizeof(payload);
    CHECK(f.call());
    CHECK(f.execution.ppu.cpu.gpr[3] == 0);
    // 'A', \x00, \x7F, \x09, '\n', 'Z'
    const char expected[] = "A\\x00\\x7F\\x09\nZ";
    CHECK(f.ring.used == sizeof(expected) - 1);
    CHECK(std::memcmp(f.ring.bytes, expected, sizeof(expected) - 1) == 0);
    CHECK(f.ring.dropped == 0);
    CHECK(f.get32(COUNT_OUT) == sizeof(payload));
    CHECK(f.control->tty_bytes == sizeof(payload));
}
#endif   // PS3_NATIVE_STARTUP_TTY

void flag_off_is_unsupported()
{
#ifndef PS3_NATIVE_STARTUP_TTY
    // Built WITHOUT the flag: syscall 403 has no dispatch, so it must reach the unsupported stop with
    // nothing recorded and no guest byte touched.
    Fixture f;
    // Memory services present but fatal: the pre-existing memory dispatch must reject syscall 403 by number
    // (never call a service), which is exactly the feature-off behaviour this case pins.
    f.services.allocate_memory = [](void *, uint64_t, uint64_t, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
    f.services.free_memory = [](void *, uint32_t) -> ps3_user_memory_service_result { std::abort(); };
    f.services.query_memory = [](void *, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
    const char text[] = "hello\n";
    f.put(BUFFER, text, sizeof(text) - 1);
    f.execution.ppu.cpu.gpr[5] = sizeof(text) - 1;
    const auto before = f.execution.ppu.cpu;
    CHECK(!f.call());
    CHECK(f.control->stop == StartupStop::UnsupportedSyscall);
    CHECK(f.control->tty_write_calls == 0 && f.control->tty_write_returns == 0 && f.control->tty_bytes == 0);
    CHECK(f.ring.used == 0 && f.get32(COUNT_OUT) == 0xdeadbeefu);
    CHECK(f.recorder.report().published_count == 0 && !f.recorder.report().batch_active);
    CHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
#else
    (void)0;   // built WITH the flag: the positive cases above already prove the sink
#endif
}
}   // namespace

extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) {
    terminal_closed = !terminal_recorder || !terminal_recorder->report().batch_active;
    const int result = pthread_mutex_trylock(terminal_guard);
    terminal_unlocked = result == 0;
    if (!result) pthread_mutex_unlock(terminal_guard);
    throw Terminal{};   // the observations above precede stack unwinding
}

int main()
{
#ifdef PS3_NATIVE_STARTUP_TTY
    hello_newline(0);   // fd 1 -> channel 0, the game's normal stdout path
    hello_newline(1);   // fd 2 -> channel 1
    zero_length_reads_nothing();
    channel_out_of_range();
    bad_pointers();
    readonly_source_is_readable();
    readonly_count_pointer_stops();
    source_outside_every_range_stops();
    null_ring();
    overflow_is_bounded();
    escaping();
#endif
    flag_off_is_unsupported();
    if (failures) {
        std::printf("STARTUP_TTY FAIL failures=%d\n", failures);
        return 1;
    }
    std::printf("STARTUP_TTY PASS ring_bytes=%zu\n", size_t(STARTUP_TTY_RING_BYTES));
    return 0;
}

/* Host behavior test for StartupChildPpu (adapters/startup_child_ppu.{h,cpp}). HOST TEST ONLY: it is not game execution,
 * not the real helper and not a target run. Real collaborators: actual startup_execution.cpp (native_startup_worker,
 * IndirectCall, lv2_syscall sc130 bridge, stop()/terminal notification), actual PPU registry with real pthread service
 * threads, actual LV2 queues/ports, actual user-memory allocator over the actual atomic domain and mapped host space,
 * actual TLS API, actual per-child recorder/budget/dispatch-stop, actual compiled-code lookup port. The compiled bodies are
 * C++ FIXTURES (address -> function in a real ps3_compiled_code_lookup); the adapter reaches them ONLY through IndirectCall.
 * Platform shim: the host threadExit throws Terminal, caught ONLY in the thread wrapper below (then pthread_exit), so the
 * adapter's terminal notification -- not the service C trampoline's post-body exit -- resolves the creator handshake. */
#include "../adapters/startup_execution.cpp"
#include "../adapters/startup_child_ppu.h"
extern "C" {
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
}
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

namespace {
struct Terminal {};
#define CHECK(value) do { if (!(value)) { std::printf("CHILD_PPU_FAIL line=%d expression=%s\n", __LINE__, #value); std::fflush(stdout); _exit(1); } } while (0)

// Finite watchdog: a lane that does not finish is a failure (exit 124).
std::atomic<long long> deadline_ms{0};
long long now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void arm(long long ms) { deadline_ms.store(now_ms() + ms); }
void watchdog() { for (;;) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); const long long d = deadline_ms.load(); if (d && now_ms() > d) { std::printf("CHILD_PPU_WATCHDOG_TIMEOUT\n"); std::fflush(stdout); _exit(124); } } }

constexpr uint32_t HOST_SIZE = 0x900000, WINDOW = 0x400000, WINDOW_END = 0x700000, SLOT_PAGE = 0x100000, SRC = SLOT_PAGE + 0x800,
    FILL = 0xA5, NO_ID_SENTINEL = 0xEE;
constexpr uint64_t QUOTA = 0x300000, TOC = UINT64_C(0x123456789abcdef0), LR = 0x00510000, G64 = 0x10000;
constexpr uint32_t ENTRY_RECEIVE = 0x00500000, ENTRY_RETURN = 0x00500100, ENTRY_UNKNOWN = 0x00500200, ENTRY_GATED = 0x00500300;
constexpr uint32_t ENTRY_EXIT = 0x00500600;   // sys_ppu_thread_exit through the real HLE dispatch (PS3_NATIVE_GAMEDATA)
constexpr uint32_t ENTRY_TAIL = 0x00500900;   // a tail-call thunk: sets g_trampoline_fn and returns to the worker
constexpr uint32_t ENTRY_ALLOC = 0x00500700;  // lv2 syscall 348 (sys_memory_allocate) through the real lv2_syscall route
constexpr uint32_t ENTRY_TZ = 0x00500800;     // lv2 syscall 144 (timezone) through the real lv2_syscall route
constexpr uint32_t ALLOC_SIZE = 0x10000, ALLOC_ALIGN = 0x10000;   // pool, 64 KiB aligned (flags 0x200)
constexpr int32_t TZ_MINUTES = -60, TZ_SUMMER_MINUTES = 30;
constexpr uint32_t PRIORITY = 249, CREATOR = 250;
// The game's third sys_ppu_thread_create child (decision 10 A): priority 2000, creator 250, stack 0x4000. Its reservation takes the
// FENCED entry point (prepare() routes allow_thread_exit through ps3_ppu_service_reserve_guest_fenced), so the real pair reserves.
constexpr uint32_t GD_PRIORITY = 2000, GD_CREATOR = 250, GD_STACK = 0x4000;

// ---------------- compiled-code fixtures (reached only through the real lookup + IndirectCall) ----------------
struct Obs {
    uint32_t thread_id = 0, queue = 0;
    uint64_t timeout = 0;
    std::atomic<bool> entered{false}, prefix{false}, resumed{false};
    std::atomic<bool> gate_reached{false}, gate_open{false}, gate_timeout{false}, second_returned{false};   // ENTRY_GATED only
    uint64_t r1 = 0, r2 = 0, r3 = 0, r4 = 0, r13 = 0, lr = 0, ctr = 0;
    uint32_t cpu_thread_id = 0;
    unsigned char id_bytes[8]{};
    ps3_guest_memory_status id_read = PS3_GUEST_MEMORY_INVALID_RANGE;
    uint64_t after[8]{}, marker_before = 0, marker_after = 0;
    BootContentMetadata boot_content_before{},boot_content_after{};
    SystemParameters system_parameters_before{},system_parameters_after{};
    size_t system_parameter_calls=0,system_parameter_returns=0;
    bool system_parameter_observation_fresh=false;
    SystemProfile system_profile_before{},system_profile_after{};
    PresentationTopology topology_before=PresentationTopology::Unconfigured,topology_after=PresentationTopology::Unconfigured;
    NativePresentationMode mode_before=NativePresentationMode::Unconfigured,mode_after=NativePresentationMode::Unconfigured;
    bool query_video_count=false,video_observation_fresh=false;
    uint64_t video_count_before=0,video_count_after=0;
    size_t video_calls_before=0,video_returns_before=0,video_calls_after=0,video_returns_after=0;
    size_t system_string_calls=0,system_string_returns=0;
    bool system_string_observation_fresh=false;
    ps3_disc_change_registry *disc_registry_before=nullptr,*disc_registry_after=nullptr;
    ps3_disc_change_pair disc_pair_before{},disc_pair_after{};
    bool use_disc_hle=false,disc_observation_fresh=false;
    size_t disc_calls_before=0,disc_returns_before=0,disc_calls_after=0,disc_returns_after=0;
    StartupDiscChangeObservation disc_registration_before{},disc_registration_after{};
    bool query_language=false;
    std::atomic<bool> exit_import_returned{false};    StartupProcessSdk sdk_before{},sdk_after{};
    uint32_t language_effective_before=0,language_effective_after=0;
    uint32_t language_value_before=0,language_value_after=0;
    const StartupControl *execution_control=nullptr;
    size_t boot_disc_calls=0,boot_disc_returns=0;
    bool boot_disc_observation_fresh=false;
    // decision 14 process-wide services: what the child's own syscall route observed
    uint64_t alloc_r3=0, alloc_published=0, alloc_size=0, tz_r3=0, tz_written_minutes=0, tz_written_summertime=0;
    ps3_user_memory_service_status alloc_service_status=PS3_USER_MEMORY_SERVICE_ARGUMENT;
    uint32_t alloc_used_before=0, alloc_used_after=0;
};
Obs *g_obs[8];
Obs *obs_for(uint32_t id) { for (Obs *o : g_obs) if (o && o->thread_id == id) return o; std::printf("CHILD_PPU_FAIL no observation for id\n"); _exit(1); }

void query_child_language(ppu_context *cpu,uint32_t &effective,uint32_t &value) {
    const uint64_t old3=cpu->gpr[3],old4=cpu->gpr[4];
    cpu->gpr[3]=0x111; cpu->gpr[4]=cpu->gpr[1]+0x60;
    ps3_hle_call(0x40e895d3,cpu);
    CHECK(cpu->gpr[3]==0 && active->system_parameter.has_result);
    effective=active->system_parameter.effective_id;
    const auto &execution=*reinterpret_cast<const StartupExecution *>(cpu);
    unsigned char bytes[4]{};
    const auto &memory=execution.ppu.memory->coherent_memory;
    CHECK(memory.read(memory.context,uint32_t(cpu->gpr[4]),bytes,4)==PS3_GUEST_MEMORY_OK);
    value=(uint32_t(bytes[0])<<24)|(uint32_t(bytes[1])<<16)|(uint32_t(bytes[2])<<8)|bytes[3];
    CHECK(active->system_parameter.value==value && active->system_parameter.output_written);
    cpu->gpr[3]=old3; cpu->gpr[4]=old4;
}
constexpr ps3_disc_change_pair DISC_ENTRY{0x11,0xfffffff3},DISC_PARENT{0x77,0x89},DISC_RESUME{0x91,0xb3};
bool same_disc_pair(ps3_disc_change_pair a,ps3_disc_change_pair b) {
    return a.eject_descriptor==b.eject_descriptor && a.insert_descriptor==b.insert_descriptor;
}
void register_child_disc_pair(ppu_context *cpu,ps3_disc_change_pair pair,StartupDiscChangeObservation &record) {
    const auto old3=cpu->gpr[3],old4=cpu->gpr[4];
    cpu->gpr[3]=pair.eject_descriptor;cpu->gpr[4]=pair.insert_descriptor;
    ps3_hle_call(0xdfdd302e,cpu);
    CHECK(cpu->gpr[3]==0 && active->disc_change.has_result && active->disc_change.toc_written);
    record=active->disc_change;
    CHECK(record.registration_attempted && record.registration.status==PS3_DISC_CHANGE_OK && record.registration.committed);
    CHECK(same_disc_pair(record.registration.current,pair));
    cpu->gpr[3]=old3;cpu->gpr[4]=old4;
}
uint64_t query_child_video_count(ppu_context *cpu) {
    const auto old3=cpu->gpr[3];
    cpu->gpr[3]=0;
    ps3_hle_call(0x75bbb672,cpu);
    const auto count=cpu->gpr[3];
    CHECK(count==1 && active->video_output_count.has_result && active->video_output_count.toc_written);
    cpu->gpr[3]=old3;
    return count;
}
void body_receive(ppu_context *cpu)
{
    Obs *o = obs_for(cpu->thread_id);
    const auto &execution=*reinterpret_cast<const StartupExecution *>(cpu);
    o->boot_content_before=execution.boot_content;
    o->system_parameters_before=execution.system_parameters;
    o->system_profile_before=execution.system_profile;
    o->topology_before=execution.presentation_topology;
    o->mode_before=execution.presentation_mode;
    o->video_calls_before=active->video_output_count_calls;o->video_returns_before=active->video_output_count_returns;
    const auto &video=active->video_output_count;
    o->video_observation_fresh=!video.raw_video_out && !video.inventory_checked && !video.has_result && !video.toc_written && !video.publications;
    if(o->query_video_count) o->video_count_before=query_child_video_count(cpu);
    o->disc_registry_before=execution.disc_change_callbacks;
    o->disc_calls_before=active->disc_change_calls;o->disc_returns_before=active->disc_change_returns;
    const auto &disc=active->disc_change;
    o->disc_observation_fresh=!disc.raw_eject && !disc.raw_insert && !disc.registration_attempted &&
        !disc.registration.committed && !disc.toc_written && !disc.has_result && !disc.publications;
    if(execution.disc_change_callbacks)
        CHECK(ps3_disc_change_read(execution.disc_change_callbacks,&o->disc_pair_before)==PS3_DISC_CHANGE_OK);
    if(o->use_disc_hle) {
        std::printf("DISC_CHILD_AUTHORITY present=%u\n",unsigned(execution.disc_change_callbacks!=nullptr));
        register_child_disc_pair(cpu,DISC_ENTRY,o->disc_registration_before);
    }
    o->system_string_calls=active->system_string_calls;
    o->system_string_returns=active->system_string_returns;
    const auto &string=active->system_string;
    o->system_string_observation_fresh=!string.has_result && !string.profile_checked &&
        !string.toc_written && !string.output_write_attempts && !string.output_written_bytes &&
        !string.publications && !string.raw_id && !string.raw_output && !string.raw_size &&
        string.source==SystemProfileSource::Unconfigured && string.profile_status==SystemProfileStatus::Unconfigured;
    o->system_parameter_calls=active->system_parameter_calls;
    o->system_parameter_returns=active->system_parameter_returns;
    const auto &parameter=active->system_parameter;
    o->system_parameter_observation_fresh=!parameter.has_result && !parameter.settings_checked &&
        !parameter.output_written && !parameter.toc_written && !parameter.output_write_attempts &&
        !parameter.publications && !parameter.raw_id && !parameter.raw_output &&
        parameter.settings.profile==SystemParameterProfile::Unavailable &&
        parameter.settings.language.status==SystemLanguageStatus::Unavailable;
    o->sdk_before=execution.process_sdk;
    if(o->query_language) query_child_language(cpu,o->language_effective_before,o->language_value_before);
    o->execution_control=active;
    o->boot_disc_calls=active->boot_disc_calls; o->boot_disc_returns=active->boot_disc_returns;
    o->boot_disc_observation_fresh=!active->boot_disc.has_result && !active->boot_disc.output_writes && !active->boot_disc.metadata_checked;
    o->r1 = cpu->gpr[1]; o->r2 = cpu->gpr[2]; o->r3 = cpu->gpr[3]; o->r4 = cpu->gpr[4]; o->r13 = cpu->gpr[13];
    o->lr = cpu->lr; o->ctr = cpu->ctr; o->cpu_thread_id = cpu->thread_id;
    PpuMemoryExecution &owner = *reinterpret_cast<PpuMemoryExecution *>(cpu);   // the real owner of this CPU
    o->id_read = owner.memory->coherent_memory.read(owner.memory->coherent_memory.context, uint32_t(cpu->gpr[3]), o->id_bytes, 8);
    volatile uint64_t marker = UINT64_C(0xA1B2C3D4E5F60718);   // a live frame local across the blocking syscall
    o->marker_before = marker;
    o->entered.store(true);
    o->prefix.store(true);   // creator-prefix work done before the real block
    cpu->gpr[11] = 130; cpu->gpr[3] = o->queue; cpu->gpr[4] = cpu->gpr[1] + 0x70; cpu->gpr[5] = o->timeout;
    lv2_syscall(cpu);
    o->boot_content_after=execution.boot_content;
    o->system_parameters_after=execution.system_parameters;
    o->system_profile_after=execution.system_profile;
    o->topology_after=execution.presentation_topology;
    o->mode_after=execution.presentation_mode;
    if(o->query_video_count) o->video_count_after=query_child_video_count(cpu);
    o->video_calls_after=active->video_output_count_calls;o->video_returns_after=active->video_output_count_returns;
    o->disc_registry_after=execution.disc_change_callbacks;
    if(execution.disc_change_callbacks)
        CHECK(ps3_disc_change_read(execution.disc_change_callbacks,&o->disc_pair_after)==PS3_DISC_CHANGE_OK);
    if(o->use_disc_hle) register_child_disc_pair(cpu,DISC_RESUME,o->disc_registration_after);
    o->disc_calls_after=active->disc_change_calls;o->disc_returns_after=active->disc_change_returns;
    o->sdk_after=execution.process_sdk;
    if(o->query_language) query_child_language(cpu,o->language_effective_after,o->language_value_after);
    for (int i = 0; i < 8; ++i) o->after[i] = cpu->gpr[3 + i];
    o->marker_after = marker;
    o->resumed.store(true);
}
void body_return(ppu_context *cpu)
{
    Obs *o = obs_for(cpu->thread_id);
    o->r1 = cpu->gpr[1]; o->r13 = cpu->gpr[13]; o->entered.store(true);
    cpu->gpr[3] = 0x5151;
}
struct CodeEntry { uint32_t address; ps3_compiled_function function; };
// Deterministic RUNNING fixture: receive once (real block, resumed by a real send), then run compiled code that performs NO
// receive and no indirect call until the test opens an explicit release latch (capped wait, never an uncapped spin), then a
// second receive: the real cancellation boundary. Reaching the second receive after a cancel must stop the child.
void body_gated(ppu_context *cpu)
{
    Obs *o = obs_for(cpu->thread_id);
    body_receive(cpu);
    o->gate_reached.store(true);
    for (int i = 0; i < 5000 && !o->gate_open.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!o->gate_open.load()) o->gate_timeout.store(true);
    cpu->gpr[11] = 130; cpu->gpr[3] = o->queue; cpu->gpr[4] = cpu->gpr[1] + 0x70; cpu->gpr[5] = 0;
    lv2_syscall(cpu);
    o->second_returned.store(true);
}
// Deterministic guest-initiated TERMINATION fixture: the real sys_ppu_thread_exit HLE import (NID 0xAFF080A4) through the real
// startup_execution dispatch, with the exit value in r3. With child_thread_exit_allowed the child stops GuestThreadExit; without
// it the import stops UnsupportedSyscall, exactly as for the primary thread.
constexpr uint32_t EXIT_VALUE = 0x0042C0DE;
void body_exit(ppu_context *cpu)
{
    Obs *o = obs_for(cpu->thread_id);
    o->entered.store(true);
    cpu->gpr[3] = EXIT_VALUE;
    ps3_hle_call(0xaff080a4, cpu);   // real HLE import dispatch: either ends this child or stops UnsupportedSyscall
    o->exit_import_returned.store(true);   // reached only if the import did not stop
}
// The real body reached only through the pending thread-local tail call. It performs the same observable work as case_thread_exit_allowed.
void body_tail_exit(void *context)
{
    body_exit(static_cast<ppu_context *>(context));
}
// The game's func_004C9A88 shape: leave the real body in g_trampoline_fn and return without running it.
void body_tail_thunk(ppu_context *)
{
    g_trampoline_fn = &body_tail_exit;
}
// decision 14, syscall 348 (sys_memory_allocate) through the REAL lv2_syscall route. A child with process_services borrows the
// process's allocate_memory/free_memory/query_memory and really allocates; a child without them stops MissingServices.
void body_alloc(ppu_context *cpu)
{
    Obs *o = obs_for(cpu->thread_id);
    o->entered.store(true);
    const uint32_t out = uint32_t(cpu->gpr[1] + 0x80);          // r1+0x80 is inside the child's own committed stack
    cpu->gpr[11] = 348; cpu->gpr[3] = ALLOC_SIZE; cpu->gpr[4] = 0x200; cpu->gpr[5] = out;
    lv2_syscall(cpu);
    o->alloc_r3 = cpu->gpr[3];
    o->alloc_service_status = active->memory_result.status;
    unsigned char be[4]{};
    const auto &execution = *reinterpret_cast<const StartupExecution *>(cpu);
    const auto &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, out, be, 4) == PS3_GUEST_MEMORY_OK)
        o->alloc_published = uint32_t(be[0]) << 24 | uint32_t(be[1]) << 16 | uint32_t(be[2]) << 8 | be[3];
}
// decision 14, syscall 144 (timezone) through the REAL lv2_syscall route. A child with fixed_timezone borrows the process's
// timezone; a child without it stops MissingServices before any write.
void body_tz(ppu_context *cpu)
{
    Obs *o = obs_for(cpu->thread_id);
    o->entered.store(true);
    const uint32_t timezone_out = uint32_t(cpu->gpr[1] + 0x80), summertime_out = uint32_t(cpu->gpr[1] + 0x84);
    cpu->gpr[11] = 144; cpu->gpr[3] = timezone_out; cpu->gpr[4] = summertime_out;
    lv2_syscall(cpu);
    o->tz_r3 = cpu->gpr[3];
    unsigned char be[4]{};
    const auto &execution = *reinterpret_cast<const StartupExecution *>(cpu);
    const auto &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, timezone_out, be, 4) == PS3_GUEST_MEMORY_OK)
        o->tz_written_minutes = int32_t(uint32_t(be[0]) << 24 | uint32_t(be[1]) << 16 | uint32_t(be[2]) << 8 | be[3]);
    if (memory.read(memory.context, summertime_out, be, 4) == PS3_GUEST_MEMORY_OK)
        o->tz_written_summertime = int32_t(uint32_t(be[0]) << 24 | uint32_t(be[1]) << 16 | uint32_t(be[2]) << 8 | be[3]);
}
const CodeEntry code_entries[] = {{ENTRY_RECEIVE, body_receive}, {ENTRY_RETURN, body_return}, {ENTRY_GATED, body_gated}, {ENTRY_EXIT, body_exit},
    {ENTRY_ALLOC, body_alloc}, {ENTRY_TZ, body_tz}, {ENTRY_TAIL, body_tail_thunk}};
ps3_compiled_code_status code_find(const void *, uint32_t address, ps3_compiled_function *target)
{
    for (const CodeEntry &e : code_entries) if (e.address == address) { *target = e.function; return PS3_COMPILED_CODE_OK; }
    return PS3_COMPILED_CODE_NOT_FOUND;
}
struct StopCounter { std::atomic<unsigned> calls{0}; };
ps3_compiled_dispatch_stop_reason stop_requested(void *context) { static_cast<StopCounter *>(context)->calls.fetch_add(1); return PS3_COMPILED_DISPATCH_CONTINUE; }
ps3_clock_status clock_now(void *, uint64_t *value) { *value = 1; return PS3_CLOCK_OK; }
// Per-child budget clock (injected seam): constant 1 until a test advances it so that the NEXT budget poll exhausts the budget.
ps3_clock_status budget_clock_now(void *c, uint64_t *value) { *value = static_cast<std::atomic<uint64_t> *>(c)->load(); return PS3_CLOCK_OK; }
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) { throw Terminal{}; }
// native_startup_worker links every OTHER entry mode (the original game helpers). This test only ever uses the IndirectCall
// entry, so these generated-code symbols are fail-stop host stand-ins: reaching one is a test failure, never a pass.
#define UNREACHABLE_ENTRY(name) void name(ppu_context *) { std::printf("CHILD_PPU_FAIL unexpected generated entry " #name "\n"); std::fflush(stdout); _exit(1); }
UNREACHABLE_ENTRY(func_0055DA64) UNREACHABLE_ENTRY(func_0055D9E4) UNREACHABLE_ENTRY(func_0055D924) UNREACHABLE_ENTRY(func_0055DAE4)
UNREACHABLE_ENTRY(func_0055D944) UNREACHABLE_ENTRY(func_0055DB24) UNREACHABLE_ENTRY(func_0055DA84) UNREACHABLE_ENTRY(func_00010230)
uint64_t ppu_timebase_now(void) { std::printf("CHILD_PPU_FAIL unexpected ppu_timebase_now\n"); std::fflush(stdout); _exit(1); }

namespace {
// ---------------- real memory ports wrapped to fill on commit and to fail a decommit once without freeing ----------------
struct MemSpy {
    ps3_guest_memory real{};      // the coherent atomic-domain memory (read/write/zero)
    ps3_atomic_domain *domain = nullptr;   // allocator commit/decommit MUST go through the domain (it owns visibility + pins)
    std::atomic<unsigned> fail_decommit{0}, decommit_calls{0}, decommit_ok{0};
};
ps3_guest_memory_status spy_commit(void *c, uint32_t a, uint64_t n)
{
    MemSpy *s = static_cast<MemSpy *>(c);
    const ps3_guest_memory_status st = ps3_atomic_commit(s->domain, a, n);
    if (st != PS3_GUEST_MEMORY_OK) return st;
    std::vector<unsigned char> pattern(n, FILL);   // a non-zero commit image proves explicit zeroing / untouched ranges
    return s->real.write(s->real.context, a, pattern.data(), n);
}
ps3_guest_memory_status spy_decommit(void *c, uint32_t a, uint64_t n)
{
    MemSpy *s = static_cast<MemSpy *>(c);
    s->decommit_calls.fetch_add(1);
    unsigned armed = s->fail_decommit.load();
    while (armed) if (s->fail_decommit.compare_exchange_weak(armed, armed - 1)) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;   // NOT freed
    const ps3_guest_memory_status st = ps3_atomic_decommit(s->domain, a, n);
    if (st == PS3_GUEST_MEMORY_OK) s->decommit_ok.fetch_add(1);
    return st;
}
ps3_guest_memory_status spy_read(void *c, uint32_t a, void *p, size_t n) { MemSpy *s = static_cast<MemSpy *>(c); return s->real.read(s->real.context, a, p, n); }
ps3_guest_memory_status spy_write(void *c, uint32_t a, const void *p, size_t n) { MemSpy *s = static_cast<MemSpy *>(c); return s->real.write(s->real.context, a, p, n); }
ps3_guest_memory_status spy_zero(void *c, uint32_t a, uint64_t n) { MemSpy *s = static_cast<MemSpy *>(c); return s->real.zero(s->real.context, a, n); }

// ---------------- real pthread service-thread adapter wrapped: Terminal catch (platform wrapper), counts, injected failures ----------------
struct ThreadRig {
    ps3_pthread_service_thread real{};
    void (*entry)(void *) = nullptr;
    void *argument = nullptr;
    bool fail_start = false;
    unsigned fail_join = 0;
    std::atomic<unsigned> start_calls{0}, join_calls{0}, join_ok{0};
};
void *terminal_wrapper(void *context)
{
    ThreadRig *t = static_cast<ThreadRig *>(context);
    try { t->entry(t->argument); } catch (const Terminal &) { pthread_exit(nullptr); }
    return nullptr;
}
void wrapper_entry(void *context) { terminal_wrapper(context); }
int thread_start(void *c, void (*entry)(void *), void *argument)
{
    ThreadRig *t = static_cast<ThreadRig *>(c);
    t->start_calls.fetch_add(1);
    if (t->fail_start) return -1;
    t->entry = entry; t->argument = argument;
    const ps3_service_thread_port real = ps3_pthread_service_thread_port(&t->real);
    return real.start(real.context, &wrapper_entry, t);
}
int thread_join(void *c)
{
    ThreadRig *t = static_cast<ThreadRig *>(c);
    t->join_calls.fetch_add(1);
    if (t->fail_join) { --t->fail_join; return -1; }
    const ps3_service_thread_port real = ps3_pthread_service_thread_port(&t->real);
    const int rc = real.join(real.context);
    if (!rc) t->join_ok.fetch_add(1);
    return rc;
}

// ---------------- per-child observation resources + the child ----------------
struct ChildRig {
    std::unique_ptr<StartupChildPpu> child{new StartupChildPpu};
    ThreadRig thread;
    StopCounter stop_counter;
    std::vector<PpuMemoryTraceEvent> events = std::vector<PpuMemoryTraceEvent>(256);
    PpuMemoryTraceRecorder recorder;
    infamous::discovery::DiscoveryBudget budget_policy{{64, 64, 64, UINT64_C(1000000000)}, 0};
    std::unique_ptr<DiscoveryBudgetBinding> budget;
    std::atomic<uint64_t> budget_time{1};
    ps3_clock budget_clock{};
    Obs obs;
    StartupGameDataPort gamedata{};   // the borrowed composition service a child observes through execution.gamedata
    uint32_t slot = 0;
};

struct Rig {
    ElfTestHostSpace *space = elf_test_host_space_create(HOST_SIZE);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    ps3_pthread_guard g_dom, g_mem, g_lv2, g_ppu;
    ps3_pthread_wait_port port;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[16]{};
    ps3_atomic_pin_record pins[128]{};
    MemSpy spy;
    ps3_user_memory memory{};
    ps3_user_memory_slot slots[8]{};
    ps3_lv2_objects lv2{};
    ps3_ppu_threads ppu{};
    PpuMemoryRamRange ranges[2]{{SLOT_PAGE, 0x1000, true}, {WINDOW, WINDOW_END - WINDOW, true}};
    std::unique_ptr<PpuMemoryBinding> binding;
    ps3_clock clock{nullptr, clock_now};
    ps3_compiled_code_lookup lookup{nullptr, code_find};
    // an admitted PRIMARY execution that must stay unaffected by every child
    std::unique_ptr<StartupControl> primary_control{new StartupControl};
    StartupExecution primary{};
    Ps3PpuExecutionServices primary_services{};
    StopCounter primary_stop;
    std::vector<PpuMemoryTraceEvent> primary_events = std::vector<PpuMemoryTraceEvent>(64);
    PpuMemoryTraceRecorder primary_recorder;
    infamous::discovery::DiscoveryBudget primary_policy{{64, 64, 64, UINT64_C(1000000000)}, 0};
    std::unique_ptr<DiscoveryBudgetBinding> primary_budget;
    // decision 14: the process-wide service instance the primary uses (real user-memory service over this rig's allocator), plus
    // the process's fixed timezone. Children borrow BOTH through ::process_services / ::fixed_timezone.
    Ps3PpuExecutionServices process_services{};
    StartupFixedTimezone process_timezone{TZ_MINUTES, TZ_SUMMER_MINUTES};
    std::atomic<unsigned> process_alloc_calls{0}, process_free_calls{0}, process_query_calls{0};
    std::vector<std::unique_ptr<ChildRig>> children;

    explicit Rig(uint64_t quota = QUOTA) {
        CHECK(space && backing.commit(backing.context, 0, HOST_SIZE) == PS3_GUEST_MEMORY_OK);
        CHECK(!ps3_pthread_guard_init(&g_dom) && !ps3_pthread_guard_init(&g_mem) && !ps3_pthread_guard_init(&g_lv2) &&
              !ps3_pthread_guard_init(&g_ppu) && !ps3_pthread_wait_port_init(&port));
        CHECK(ps3_atomic_domain_init(&domain, backing, ps3_pthread_guard_port(&g_dom), owners, 16, pins, 128) == PS3_GUEST_MEMORY_OK);
        spy.real = ps3_atomic_domain_memory(&domain); spy.domain = &domain;
        const ps3_guest_memory spied{&spy, spy_commit, spy_decommit, spy_read, spy_write, spy_zero};
        const ps3_user_memory_config mc{WINDOW, WINDOW_END, quota, spied, ps3_pthread_guard_port(&g_mem)};
        CHECK(ps3_user_memory_init(&memory, &mc, slots, 8) == PS3_USER_MEMORY_OK);
        CHECK(ps3_lv2_objects_init(&lv2, ps3_pthread_guard_port(&g_lv2), ps3_pthread_wait_port_port(&port)) == PS3_LV2_OK);
        CHECK(ps3_ppu_threads_init(&ppu, ps3_pthread_guard_port(&g_ppu), ps3_pthread_wait_port_port(&port), &memory) == PS3_PPU_OK);
        CHECK(ps3_ppu_threads_adopt_main(&ppu, 1) == PS3_PPU_OK);
        binding.reset(new PpuMemoryBinding(domain, ranges, 2));
        CHECK(ps3_atomic_commit(&domain, SLOT_PAGE, 0x1000) == PS3_GUEST_MEMORY_OK);
        lookup.context = nullptr;
        const unsigned char src[4]{0x11, 0x22, 0x33, 0x44};
        put(SRC, src, 4);
        // primary: its own control/services/recorder/budget, admitted through the same real admission
        primary_services.memory = &binding->coherent_memory; primary_services.clock = &clock; primary_services.compiled_code = &lookup;
        primary.control = primary_control.get();
        primary.dispatch = {0, 8}; primary.dispatch_stop = {&primary_stop, stop_requested};
        primary.ppu.cpu.thread_id = 1;
        CHECK(primary_recorder.configure(primary_events.data(), primary_events.size(), PpuMemoryTraceRetention::Strict, 0));
        primary_budget.reset(new DiscoveryBudgetBinding(primary_policy, clock));
        CHECK(native_startup_admit_budgeted(primary, *binding, primary_services, primary_recorder, *primary_budget));
        // The process-wide instance the primary's own syscalls use: the REAL guarded user-memory service over this rig's pool.
        process_services = Ps3PpuExecutionServices{};
        process_services.context = this;
        process_services.create_mutex = &Rig::create_mutex;
        process_services.lock_mutex = &Rig::lock_mutex;
        process_services.try_mutex = &Rig::try_mutex;
        process_services.unlock_mutex = &Rig::unlock_mutex;
        process_services.destroy_mutex = &Rig::destroy_mutex;
        process_services.allocate_memory = &Rig::allocate_memory;
        process_services.free_memory = &Rig::free_memory;
        process_services.query_memory = &Rig::query_memory;
        process_services.memory = &binding->coherent_memory;
        process_services.clock = &clock; process_services.compiled_code = &lookup;
    }
    static ps3_lwmutex_result create_mutex(void *, uint32_t, const Ps3PpuLwmutexAttributes *)
    { ps3_lwmutex_result r{}; r.status = PS3_LWMUTEX_UNSUPPORTED; return r; }
    static ps3_lwmutex_result lock_mutex(void *, uint32_t, ps3_lwmutex_thread, uint64_t, ps3_wait_cancel)
    { ps3_lwmutex_result r{}; r.status = PS3_LWMUTEX_UNSUPPORTED; return r; }
    static ps3_lwmutex_result try_mutex(void *, uint32_t, ps3_lwmutex_thread)
    { ps3_lwmutex_result r{}; r.status = PS3_LWMUTEX_UNSUPPORTED; return r; }
    static ps3_lwmutex_result unlock_mutex(void *, uint32_t, ps3_lwmutex_thread)
    { ps3_lwmutex_result r{}; r.status = PS3_LWMUTEX_UNSUPPORTED; return r; }
    static ps3_lwmutex_result destroy_mutex(void *, uint32_t)
    { ps3_lwmutex_result r{}; r.status = PS3_LWMUTEX_UNSUPPORTED; return r; }
    // The process-wide user-memory service fixture: the REAL allocator over this rig's pool, publishing the addressed guest
    // output as a big-endian u32 exactly like the production ABI adapter (which this verifier does not link).
    static ps3_user_memory_service_result publish_be32(Rig &r, uint32_t address, uint32_t value, ps3_user_memory_service_result out)
    {
        const ps3_guest_memory &m = r.binding->coherent_memory;
        const unsigned char be[4]{uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
        if (out.status != PS3_USER_MEMORY_SERVICE_OK) return out;
        out.memory_status = m.write(m.context, address, be, 4);
        if (out.memory_status == PS3_GUEST_MEMORY_OK) { out.published = 1; out.retained_allocation = 1; out.retained_address = address; }
        else out.status = PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE;
        return out;
    }
    static ps3_user_memory_service_result allocate_memory(void *c, uint64_t size, uint64_t alignment, uint64_t output)
    {
        Rig &r = *static_cast<Rig *>(c);
        r.process_alloc_calls.fetch_add(1);
        ps3_user_memory_service_result out{};
        if (!size || !output || output > UINT32_MAX) { out.status = PS3_USER_MEMORY_SERVICE_ARGUMENT; return out; }
        uint32_t address = 0;
        const ps3_user_memory_result a = ps3_user_memory_allocate(&r.memory, size, alignment, &address);
        out.allocator = a;
        if (a.status != PS3_USER_MEMORY_OK) { out.status = PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE; return out; }
        out.status = PS3_USER_MEMORY_SERVICE_OK;
        return publish_be32(r, uint32_t(output), address, out);
    }
    static ps3_user_memory_service_result free_memory(void *c, uint32_t address)
    {
        Rig &r = *static_cast<Rig *>(c);
        r.process_free_calls.fetch_add(1);
        ps3_user_memory_service_result out{};
        out.allocator = ps3_user_memory_free(&r.memory, address);
        out.status = out.allocator.status == PS3_USER_MEMORY_OK ? PS3_USER_MEMORY_SERVICE_OK : PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE;
        return out;
    }
    static ps3_user_memory_service_result query_memory(void *c, uint64_t output)
    {
        Rig &r = *static_cast<Rig *>(c);
        r.process_query_calls.fetch_add(1);
        ps3_user_memory_service_result out{};
        if (!output || output > UINT32_MAX) { out.status = PS3_USER_MEMORY_SERVICE_ARGUMENT; return out; }
        ps3_user_memory_info info{};
        if (ps3_user_memory_query(&r.memory, &info) != PS3_USER_MEMORY_OK) { out.status = PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE; return out; }
        out.status = PS3_USER_MEMORY_SERVICE_OK;
        return publish_be32(r, uint32_t(output), uint32_t(info.available_bytes > UINT32_MAX ? UINT32_MAX : info.available_bytes), out);
    }
    ~Rig() {
        CHECK(primary.ppu.admitted && native_startup_release_after_join(primary) == PS3_GUEST_MEMORY_OK);
        binding.reset();
        CHECK(ps3_user_memory_cleanup(&memory).status == PS3_USER_MEMORY_OK);
        ps3_pthread_wait_port_destroy(&port);
        ps3_pthread_guard_destroy(&g_dom); ps3_pthread_guard_destroy(&g_mem); ps3_pthread_guard_destroy(&g_lv2); ps3_pthread_guard_destroy(&g_ppu);
        elf_test_host_space_destroy(space);
    }
    ps3_guest_memory mem() { return binding->coherent_memory; }
    void put(uint32_t a, const void *b, size_t n) { CHECK(mem().write(mem().context, a, b, n) == PS3_GUEST_MEMORY_OK); }
    void get(uint32_t a, void *b, size_t n) { const ps3_guest_memory_status st = mem().read(mem().context, a, b, n); if (st != PS3_GUEST_MEMORY_OK) std::printf("read 0x%x+0x%zx status=%d\n", a, n, int(st)); CHECK(st == PS3_GUEST_MEMORY_OK); }
    bool all(uint32_t a, uint64_t n, unsigned char v) {
        std::vector<unsigned char> b(n);
        for (uint64_t off = 0; off < n; off += 0x1000) get(a + uint32_t(off), b.data() + off, std::min<uint64_t>(0x1000, n - off));
        for (unsigned char x : b) if (x != v) return false;
        return true;
    }
    uint64_t used() { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&memory, &i) == PS3_USER_MEMORY_OK); return i.used_bytes; }
    size_t allocations() { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&memory, &i) == PS3_USER_MEMORY_OK); return i.allocation_count; }
    uint32_t queue_create() {
        uint32_t id = 0; const ps3_lv2_queue_attr attr{PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0x7175657565ull};
        CHECK(ps3_lv2_queue_create(&lv2, &id, &attr, 0, 4) == PS3_LV2_OK); return id;
    }
    uint32_t send(uint32_t queue, uint64_t d1, uint64_t d2, uint64_t d3) {   // real event port -> queue
        uint32_t p = 0;
        const uint32_t created = ps3_lv2_port_create(&lv2, &p, PS3_EVENT_PORT_LOCAL, 0);
        const uint32_t connected = created == PS3_LV2_OK ? ps3_lv2_port_connect_local(&lv2, p, queue) : 0;
        if (created != PS3_LV2_OK || connected != PS3_LV2_OK) std::printf("port create=%x connect=%x\n", created, connected);
        CHECK(created == PS3_LV2_OK && connected == PS3_LV2_OK);
        CHECK(ps3_lv2_port_send(&lv2, p, d1, d2, d3) == PS3_LV2_OK);
        return p;
    }
    ChildRig &new_child(uint32_t entry_queue = 0, bool recorder_configured = true) {
        children.emplace_back(new ChildRig);
        ChildRig &c = *children.back();
        c.slot = SLOT_PAGE + 8 * uint32_t(children.size() - 1);
        c.obs.queue = entry_queue;
        if (recorder_configured) CHECK(c.recorder.configure(c.events.data(), c.events.size(), PpuMemoryTraceRetention::Strict, 0));
        c.budget_clock = ps3_clock{&c.budget_time, budget_clock_now};
        c.budget.reset(new DiscoveryBudgetBinding(c.budget_policy, c.budget_clock));
        c.thread.real = {};
        return c;
    }
    StartupChildPpuPorts ports(ChildRig &c) {
        StartupChildPpuPorts p;
        p.threads = &ppu; p.queues = &lv2; p.memory = binding.get();
        p.thread = {&c.thread, thread_start, thread_join};
        p.code = &lookup; p.clock = &clock; p.recorder = &c.recorder; p.budget = c.budget.get();
        p.dispatch_stop = {&c.stop_counter, stop_requested}; p.dispatch_limit = 8;
        return p;
    }
    StartupChildPpuRequest request(ChildRig &c, uint32_t entry = ENTRY_RECEIVE) {
        StartupChildPpuRequest r;
        r.kind = 1; r.priority = PRIORITY; r.creator_priority = CREATOR; r.stack_size = G64; r.tls_capacity = G64;
        r.entry_address = entry; r.toc = TOC; r.argument = c.slot; r.return_lr = LR; r.stack_entry_slack = 0x70;
        r.tls_template.source = SRC; r.tls_template.file_size = 4; r.tls_template.memory_size = 0x200;
        r.gamedata = &c.gamedata;
        return r;
    }
    void write_id(ChildRig &c) {   // the CALLER's job between prepare and start: BE u64 thread id
        const uint64_t id = c.child->id();
        unsigned char be[8]; for (int i = 0; i < 8; ++i) be[i] = uint8_t(id >> (56 - 8 * i));
        put(c.slot, be, 8);
    }
    // prepare (+ optional id write). Registers the observation under the child's real id.
    void prepare_ok(ChildRig &c, uint32_t entry = ENTRY_RECEIVE, bool write = true) {
        unsigned char sentinel[8]; memset(sentinel, NO_ID_SENTINEL, 8); put(c.slot, sentinel, 8);
        const StartupChildPpuPrepareResult r = c.child->prepare(ports(c), request(c, entry));
        CHECK(r.status == StartupChildPpuPrepareStatus::Ok && r.owns_resources && c.child->id() != 0);
        c.obs.thread_id = c.child->id();
        for (Obs *&o : g_obs) if (!o) { o = &c.obs; break; }
        if (write) write_id(c);
    }
    bool exited(ChildRig &c) {
        const StartupChildPpuStatus s = c.child->status();
        return s.terminal_calls >= 1 && __atomic_load_n(&ppu.services[s.ref.slot].activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_EXITED;
    }
    void wait_exited(ChildRig &c) { arm(10000); while (!exited(c)) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    void reap_ok(ChildRig &c) {   // bounded poll: StillRunning is the honest answer until the real activity has exited
        arm(10000);
        StartupChildPpuReapResult r = c.child->reap();
        while (r.status == StartupChildPpuReapStatus::StillRunning) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); r = c.child->reap(); }
        CHECK(r.status == StartupChildPpuReapStatus::Released);
        const StartupChildPpuStatus s = c.child->status();
        CHECK(!s.has_resources && !s.service_owned && !s.admitted && s.phase == StartupChildPpuPhase::Released);
    }
    bool primary_events_untouched() {
        std::vector<PpuMemoryTraceEvent> zero(primary_events.size());
        return !memcmp(zero.data(), primary_events.data(), zero.size() * sizeof(PpuMemoryTraceEvent));
    }
    void check_primary_untouched() {
        CHECK(primary.ppu.admitted && primary_control->stop == StartupStop::None && primary_control->dispatch_count == 0);
        CHECK(primary_stop.calls.load() == 0 && primary_control->import_count == 0 && primary_events_untouched());   // never ran, never touched by a child
    }
};
void clear_obs() { for (Obs *&o : g_obs) o = nullptr; }
bool same_boot_content(const BootContentMetadata &left,const BootContentMetadata &right) {
    return left.origin==right.origin && left.parental_level==right.parental_level &&
        !std::memcmp(left.title_id,right.title_id,sizeof(left.title_id));
}
BootContentMetadata boot_content_fixture(BootContentOrigin origin) {
    BootContentMetadata metadata;
    metadata.origin=origin;
    if (origin!=BootContentOrigin::Unavailable) {
        const char title[10]="BCUS98119";
        std::memcpy(metadata.title_id,title,sizeof(title)); metadata.parental_level=5;
    }
    return metadata;
}
uint64_t be64(const unsigned char *b) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = v << 8 | b[i]; return v; }
void check_released_baseline(Rig &r) { CHECK(r.used() == 0 && r.allocations() == 0 && ps3_ppu_threads_live(&r.ppu) == 0); }

// ============ 1. exact prepared state: layout, TLS, header, BSS, padding, CPU seed; id written before start ============
void case_prepare_layout_and_cpu() {
    Rig r; ChildRig &c = r.new_child(0);
    r.prepare_ok(c, ENTRY_RECEIVE, false);
    const StartupChildPpuStatus s = c.child->status();
    CHECK(s.phase == StartupChildPpuPhase::Reserved && s.has_resources && s.service_owned && s.admitted && !s.host_started);
    CHECK(s.storage.allocation_bytes == 0x20000 && s.storage.stack_region_bytes == G64 && s.storage.tls_capacity_bytes == G64);
    CHECK(s.storage.tls_address == s.storage.stack_address + G64 && s.storage.allocation_address % G64 == 0);
    CHECK(r.used() == 0x20000 && r.allocations() == 1);
    const uint32_t A = s.storage.stack_address, R = s.storage.tls_address;
    // stack: untouched commit pattern
    CHECK(r.all(A, G64, FILL));
    // TLS: padding below header untouched, header zero, image copied, BSS zero, tail untouched
    CHECK(r.all(R, 0xFD0, FILL) && r.all(R + 0xFD0, 0x30, 0));
    unsigned char image[4]; r.get(R + 0x1000, image, 4);
    CHECK(image[0] == 0x11 && image[1] == 0x22 && image[2] == 0x33 && image[3] == 0x44);
    CHECK(r.all(R + 0x1004, 0x1FC, 0) && r.all(R + 0x1200, G64 - 0x1200, FILL));
    CHECK(s.tls.block == R + 0x1000 && s.tls.thread_pointer == R + 0x8000 && s.tls.allocation_size == 0x9000);
    // the thread id output is NOT written by prepare: still the sentinel
    unsigned char slot[8]; r.get(c.slot, slot, 8);
    for (unsigned char b : slot) CHECK(b == NO_ID_SENTINEL);
    c.child->cancel();   // reserved only: never started; cancel is harmless
    r.reap_ok(c);        // reserved -> abandon + admission release
    check_released_baseline(r);
    CHECK(c.thread.start_calls.load() == 0 && c.thread.join_calls.load() == 0);   // no host thread was ever involved
    r.check_primary_untouched();
    clear_obs();
}

// ============ 2. blocked at a real empty queue, creator-prefix handshake, host send resumes the SAME frame ============
void case_block_then_resume() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c);
    const StartupChildPpuStatus pre = c.child->status();
    const StartupChildPpuStartResult started = c.child->start();
    CHECK(started.status == StartupChildPpuStartStatus::Blocked && started.native_rc == PS3_PPU_OK);
    CHECK(c.obs.prefix.load() && !c.obs.resumed.load());   // prefix ran before the creator was released; the frame is parked
    CHECK(ps3_ppu_service_is_blocked(&r.ppu, pre.ref));
    const StartupChildPpuStatus blocked = c.child->status();
    CHECK(blocked.phase == StartupChildPpuPhase::Started && blocked.host_started && blocked.receive_calls == 1 && blocked.last_queue == q);
    CHECK(blocked.last_pc == ENTRY_RECEIVE && blocked.terminal_calls == 0);
    // real CPU seed observed from inside the compiled body (through IndirectCall)
    CHECK(c.obs.r1 == uint64_t(pre.storage.stack_address) + 0x10000 - 0x70 && c.obs.r1 % 16 == 0);
    CHECK(c.obs.r2 == TOC && c.obs.r3 == c.slot && c.obs.r4 == 0 && c.obs.r13 == pre.tls.thread_pointer);
    CHECK(c.obs.lr == LR && c.obs.ctr == ENTRY_RECEIVE && c.obs.cpu_thread_id == pre.ref.id);
    // id written BEFORE start and read back by the body from the real guest bytes
    CHECK(c.obs.id_read == PS3_GUEST_MEMORY_OK && be64(c.obs.id_bytes) == pre.ref.id);
    CHECK(c.stop_counter.calls.load() >= 1);   // per-child dispatch stop port was used
    // a SECOND wait-state observation: queue still empty and one receiver registered
    ps3_lv2_queue snap; CHECK(ps3_lv2_queue_snapshot(&r.lv2, q, &snap) == PS3_LV2_OK && snap.count == 0 && snap.blocked_receivers == 1);
    // stack and TLS bytes unchanged while blocked (dummy buffer r1+0x70 was never written)
    CHECK(r.all(pre.storage.stack_address, G64, FILL));
    CHECK(r.all(pre.storage.tls_address + 0x1200, G64 - 0x1200, FILL));
    // reap of a live, uncancelled child does NOT join (it would hang)
    CHECK(c.child->reap().status == StartupChildPpuReapStatus::StillRunning && c.thread.join_calls.load() == 0);
    r.send(q, 0x1111, 0x2222, 0x3333);
    r.wait_exited(c);
    const StartupChildPpuStatus done = c.child->status();
    CHECK(c.obs.resumed.load() && c.obs.marker_after == c.obs.marker_before);   // same frame resumed
    CHECK(c.obs.after[0] == 0);                                                  // r3 = CELL_OK from the real event
    CHECK(c.obs.after[2] == 0x1111 && c.obs.after[3] == 0x2222 && c.obs.after[4] == 0x3333);   // r5..r7 = data1..3 (r4 = source)
    CHECK(done.terminal_calls == 1 && done.stop == StartupStop::Returned && done.receive_calls == 1 && done.last_native_rc == PS3_LV2_OK);
    CHECK(done.fault == StartupChildPpuReceiveFault::None && !done.discarded_event);
    CHECK(done.consumed_events == 1 && done.first_consumed.data1 == 0x1111 && done.first_consumed.data2 == 0x2222 && done.first_consumed.data3 == 0x3333);
    CHECK(!ps3_ppu_service_is_blocked(&r.ppu, pre.ref));
    CHECK(r.used() == 0x20000);   // storage owned until the explicit reap
    r.reap_ok(c);
    CHECK(c.thread.join_calls.load() == 1 && c.thread.join_ok.load() == 1);
    CHECK(c.child->status().exit_code == uint32_t(StartupStop::Returned) && c.child->status().terminal_calls == 1);   // exactly once, also after join
    check_released_baseline(r); r.check_primary_untouched();
    clear_obs();
}

// ============ 3. caller did NOT write the id: the adapter never invents one ============
void case_id_not_invented() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c, ENTRY_RECEIVE, false);
    CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
    for (unsigned char b : c.obs.id_bytes) CHECK(b == NO_ID_SENTINEL);
    c.child->cancel(); r.wait_exited(c); r.reap_ok(c);
    check_released_baseline(r); clear_obs();
}

// ============ 4. cancellation while blocked: terminates, never continues, no guest ECANCELED ============
void case_cancel_blocked() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c);
    CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
    c.child->cancel(); c.child->cancel();   // idempotent
    r.wait_exited(c);
    const StartupChildPpuStatus s = c.child->status();
    CHECK(s.stop == StartupStop::Cancelled && s.terminal_calls == 1 && s.cancel_requested);
    CHECK(!c.obs.resumed.load());   // no guest instruction after the syscall ran, no error fed back
    CHECK(s.fault == StartupChildPpuReceiveFault::Cancelled && !s.discarded_event && s.consumed_events == 0);
    CHECK(r.ppu.services[s.ref.slot].exit_code == uint32_t(StartupStop::Cancelled));
    r.reap_ok(c);
    CHECK(c.thread.join_ok.load() == 1);
    check_released_baseline(r); r.check_primary_untouched(); clear_obs();
}

// ============ 5. P2-1: cancel before start. Empty cancel is a documented no-op; a Reserved cancel stops start() ============
void case_cancel_before_start() {
    Rig r; const uint32_t q = r.queue_create();
    r.send(q, 1, 2, 3);
    ChildRig &c = r.new_child(q);
    c.child->cancel();   // Empty: no-op, NOT remembered
    CHECK(!c.child->status().cancel_requested && c.child->status().phase == StartupChildPpuPhase::Empty);
    r.prepare_ok(c);
    const StartupChildPpuStatus pre = c.child->status();
    c.child->cancel();   // Reserved: remembered
    CHECK(c.child->status().cancel_requested);
    StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::Cancelled && s.native_rc == 0 && s.stop == StartupStop::None);
    s = c.child->start();   // stays refused, still no host involvement
    CHECK(s.status == StartupChildPpuStartStatus::Cancelled);
    const StartupChildPpuStatus st = c.child->status();
    CHECK(st.phase == StartupChildPpuPhase::Reserved && !st.host_started && st.service_owned && st.admitted && st.has_resources);
    CHECK(st.terminal_calls == 0 && st.receive_calls == 0 && st.consumed_events == 0 && !st.discarded_event);
    CHECK(c.thread.start_calls.load() == 0 && !c.obs.entered.load() && !c.obs.prefix.load());   // the compiled body never ran
    ps3_lv2_queue snap; CHECK(ps3_lv2_queue_snapshot(&r.lv2, q, &snap) == PS3_LV2_OK && snap.count == 1);   // event untouched
    CHECK(r.all(pre.storage.stack_address, G64, FILL) && r.all(pre.storage.tls_address + 0x1200, G64 - 0x1200, FILL));
    r.reap_ok(c);   // Reserved -> abandon + admission release
    CHECK(c.thread.start_calls.load() == 0 && c.thread.join_calls.load() == 0);
    check_released_baseline(r);
    {   // the Empty no-op really leaves a later prepare()+start() uncancelled
        ChildRig &d = r.new_child(0);
        d.child->cancel();
        r.prepare_ok(d, ENTRY_RETURN);
        CHECK(!d.child->status().cancel_requested);
        const StartupChildPpuStartResult ds = d.child->start();
        CHECK(ds.status == StartupChildPpuStartStatus::Returned && ds.stop == StartupStop::Returned);
        r.wait_exited(d); r.reap_ok(d);
    }
    check_released_baseline(r); r.check_primary_untouched(); clear_obs();
}

// ============ 6. event/cancel race: every outcome is one of three exactly-described states ============
void case_race_event_vs_cancel() {
    unsigned completed = 0, cancelled = 0;
    for (int i = 0; i < 24; ++i) {
        Rig r; const uint32_t q = r.queue_create();
        ChildRig &c = r.new_child(q);
        r.prepare_ok(c);
        CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
        std::thread sender([&] { r.send(q, 7, 8, 9); });
        c.child->cancel();
        sender.join();
        r.wait_exited(c);
        const StartupChildPpuStatus s = c.child->status();
        ps3_lv2_queue snap; CHECK(ps3_lv2_queue_snapshot(&r.lv2, q, &snap) == PS3_LV2_OK);
        if (c.obs.resumed.load()) {   // delivered before cancellation won
            ++completed;
            CHECK(s.stop == StartupStop::Returned && c.obs.after[2] == 7 && c.obs.after[3] == 8 && c.obs.after[4] == 9 && !s.discarded_event);
            CHECK(s.consumed_events == 1 && s.first_consumed.data1 == 7);
        } else {
            ++cancelled;
            CHECK(s.stop == StartupStop::Cancelled);
            if (s.discarded_event) CHECK(snap.count == 0 && s.consumed_events == 1 && s.discarded.data1 == 7 && s.discarded.data2 == 8 && s.discarded.data3 == 9);
            // a dequeue is never lost from the record: consumed (discarded OR dequeued-then-stopped by the bridge) xor still queued xor the
            // send itself lost the race to the cancelled receiver
            CHECK(s.consumed_events <= 1 && snap.count + s.consumed_events <= 1);
            if (s.consumed_events) CHECK(snap.count == 0 && s.first_consumed.data1 == 7 && s.first_consumed.data2 == 8 && s.first_consumed.data3 == 9);
        }
        r.reap_ok(c); check_released_baseline(r); clear_obs();
    }
    CHECK(completed + cancelled == 24);
}

// ============ 7. unsupported / faulting receive terminates through the real stop path with the native rc recorded ============
void receive_failure(uint32_t queue, uint64_t timeout, StartupChildPpuReceiveFault fault, uint32_t expected_rc, bool spu_queue = false) {
    Rig r;
    uint32_t q = queue;
    if (spu_queue) { const ps3_lv2_queue_attr attr{PS3_SYNC_FIFO, PS3_SPU_QUEUE, 1}; CHECK(ps3_lv2_queue_create(&r.lv2, &q, &attr, 0, 4) == PS3_LV2_OK); }
    ChildRig &c = r.new_child(q);
    c.obs.timeout = timeout;
    r.prepare_ok(c);
    const StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::Terminal && s.stop == StartupStop::UnsupportedSyscall);   // never Blocked
    CHECK(!c.obs.resumed.load());
    const StartupChildPpuStatus st = c.child->status();
    CHECK(st.fault == fault && st.last_native_rc == expected_rc && st.terminal_calls == 1 && st.receive_calls == 1 && st.last_queue == q);
    r.reap_ok(c); check_released_baseline(r); clear_obs();
}
void case_receive_faults() {
    receive_failure(0x1234, 0, StartupChildPpuReceiveFault::UnknownQueue, PS3_LV2_ESRCH);
    receive_failure(0, 1000, StartupChildPpuReceiveFault::Timeout, 0);   // queue id irrelevant: timeout is rejected first
    receive_failure(0, 0, StartupChildPpuReceiveFault::WrongQueueType, 0, true);
}

// ============ 8. unknown compiled entry: the real lookup fails on the real entry (DispatchFailure), resources owned ============
void case_unknown_entry() {
    Rig r; ChildRig &c = r.new_child(0);
    r.prepare_ok(c, ENTRY_UNKNOWN);
    const StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::Terminal && s.stop == StartupStop::DispatchFailure);
    CHECK(c.child->status().has_resources && c.child->status().terminal_calls == 1);
    r.wait_exited(c); r.reap_ok(c); check_released_baseline(r); clear_obs();
}

// ============ 9. immediate normal return: Returned, terminal exactly once ============
void case_returns() {
    Rig r; ChildRig &c = r.new_child(0);
    r.prepare_ok(c, ENTRY_RETURN);
    const StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::Returned && s.stop == StartupStop::Returned);
    CHECK(c.obs.entered.load() && c.obs.r13 == c.child->status().tls.thread_pointer);
    r.wait_exited(c);
    CHECK(c.child->status().terminal_calls == 1);
    r.reap_ok(c); CHECK(c.child->status().terminal_calls == 1); check_released_baseline(r); clear_obs();
}

// ============ 24. run-to-exit: sys_ppu_thread_exit through the REAL HLE import, gated by child_thread_exit_allowed ============
// A prepared child observes the borrowed composition game-data port through execution.gamedata (the same request member the create
// bridge copies), and either really ends itself (allowed) or stops UnsupportedSyscall exactly like the primary thread.
void case_thread_exit_allowed() {
    Rig r;
    gamedata_check state{}; gamedata_storage_port storage{}; storage.context = &state;
    for (const bool allow : {true, false}) {
        ChildRig &c = r.new_child(0);
        c.gamedata.state = &state; c.gamedata.storage = storage;
        CHECK(c.child->poll_exit().status == StartupChildPpuStartStatus::NotReserved);   // Empty before prepare
        StartupChildPpuRequest request = r.request(c, ENTRY_EXIT);
        request.allow_thread_exit = allow;
        // The real game request {priority 2000, creator 250, stack 0x4000}: with allow_thread_exit set, prepare() reserves through
        // the FENCED entry point and this lower priority is admitted; without it, the unfenced entry point still refuses it.
        request.priority = GD_PRIORITY; request.creator_priority = GD_CREATOR; request.stack_size = GD_STACK;
        CHECK(c.child->poll_exit().status == StartupChildPpuStartStatus::NotReserved);   // still Empty
        // allow -> the fenced reservation admits the lower priority; !allow -> the unfenced one still refuses it (UNSUPPORTED)
        // BEFORE anything is charged or started, which is exactly the fence: only a run-to-exit owner may take this child.
        CHECK(c.child->prepare(r.ports(c), request).status ==
            (allow ? StartupChildPpuPrepareStatus::Ok : StartupChildPpuPrepareStatus::ReserveFailed));
        if (!allow) {
            const StartupChildPpuStatus refused = c.child->status();
            CHECK(refused.phase == StartupChildPpuPhase::Empty && !refused.has_resources && !refused.service_owned && refused.ref.id == 0);
            CHECK(!c.obs.entered.load() && c.thread.start_calls.load() == 0);
            CHECK(r.used() == 0 && r.allocations() == 0);
            r.prepare_ok(c, ENTRY_EXIT);   // the fixture's default policy (249/250) for the rest of the unfenced arm
        }
        c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
        CHECK(c.child->poll_exit().status == StartupChildPpuStartStatus::NotReserved);   // Reserved, not Started
        const StartupChildPpuStartResult s = c.child->start();
        CHECK(c.obs.entered.load() && !c.obs.exit_import_returned.load());   // the import never returned to guest code
        const StartupChildPpuStatus st = c.child->status();
        if (allow) {
            CHECK(s.status == StartupChildPpuStartStatus::Exited && s.stop == StartupStop::GuestThreadExit);
            CHECK(st.stop == StartupStop::GuestThreadExit && st.terminal_calls == 1 && st.consumed_events == 0 && st.receive_calls == 0);
            r.wait_exited(c);   // poll_exit agrees once the real activity is EXITED, with no side effects
            const StartupChildPpuStartResult again = c.child->poll_exit();
            CHECK(again.status == StartupChildPpuStartStatus::Exited && again.stop == StartupStop::GuestThreadExit);
            CHECK(c.child->status().phase == StartupChildPpuPhase::Started);
        } else {
            CHECK(s.status == StartupChildPpuStartStatus::Terminal);   // never Exited
            CHECK(s.stop == StartupStop::UnsupportedSyscall && st.stop == StartupStop::UnsupportedSyscall);
            r.wait_exited(c);
            CHECK(c.child->poll_exit().status == StartupChildPpuStartStatus::Terminal);   // never Exited
        }
        r.reap_ok(c);   // an Exited child is in phase Started with activity EXITED: join, never StillRunning
        CHECK(c.thread.join_calls.load() == 1 && c.thread.join_ok.load() == 1);
        check_released_baseline(r);
        clear_obs();
    }
    r.check_primary_untouched();
}

// ============ 25. the SAME real {2000, 250, 0x4000} request is still refused at prepare WITHOUT run_to_exit ============
// prepare() then takes the unfenced reservation, whose priority rule is unchanged (PS3_PPU_UNSUPPORTED): nothing is reserved and
// nothing runs. This is the fence's other half: only a run-to-exit owner may reserve a lower-priority child.
void case_lower_priority_not_fenced() {
    Rig r; ChildRig &c = r.new_child(0);
    StartupChildPpuRequest request = r.request(c, ENTRY_EXIT);
    request.priority = GD_PRIORITY; request.creator_priority = GD_CREATOR; request.stack_size = GD_STACK;
    CHECK(!request.allow_thread_exit);   // the precondition of this case
    const StartupChildPpuPrepareResult x = c.child->prepare(r.ports(c), request);
    CHECK(x.status == StartupChildPpuPrepareStatus::ReserveFailed && x.native_rc == PS3_PPU_UNSUPPORTED && !x.owns_resources);
    const StartupChildPpuStatus s = c.child->status();
    CHECK(s.phase == StartupChildPpuPhase::Empty && !s.has_resources && !s.service_owned && s.ref.id == 0);
    CHECK(c.thread.start_calls.load() == 0 && !c.obs.entered.load());   // nothing ran
    CHECK(r.used() == 0 && r.allocations() == 0 && ps3_ppu_threads_live(&r.ppu) == 0);   // no record was reserved
    // The fenced route (allow_thread_exit) admits exactly the same request in the same object: nothing was retained above.
    request.allow_thread_exit = true;
    const StartupChildPpuPrepareResult fenced = c.child->prepare(r.ports(c), request);
    CHECK(fenced.status == StartupChildPpuPrepareStatus::Ok && fenced.owns_resources && c.child->id() != 0);
    CHECK(c.child->status().storage.stack_logical_bytes == GD_STACK);
    c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
    const StartupChildPpuStartResult st = c.child->start();
    CHECK(st.status == StartupChildPpuStartStatus::Exited && st.stop == StartupStop::GuestThreadExit);
    CHECK(c.obs.entered.load() && !c.obs.exit_import_returned.load());
    r.wait_exited(c); r.reap_ok(c);
    check_released_baseline(r);
    clear_obs();
}

// ============ 26. background child: a lower priority than its creator starts without the prefix wait ============
// A background child does not outrank its creator, so prepare() uses the fenced reservation and start() returns as soon as the
// host thread started. The child then ends itself through the real sys_ppu_thread_exit import; poll_exit() observes Exited.
void case_background_start() {
    Rig r; ChildRig &c = r.new_child(0);
    StartupChildPpuRequest request = r.request(c, ENTRY_EXIT);
    request.priority = 1001; request.creator_priority = 250; request.stack_size = GD_STACK;
    request.allow_thread_exit = true;
    request.background = true;
    CHECK(c.child->prepare(r.ports(c), request).status == StartupChildPpuPrepareStatus::Ok);
    CHECK(c.child->status().storage.stack_logical_bytes == GD_STACK);
    c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
    const StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::Background);
    CHECK(c.child->status().phase == StartupChildPpuPhase::Started && c.child->status().host_started);
    r.wait_exited(c);   // the child really ran to its own exit; start() did not wait for it
    CHECK(c.obs.entered.load() && !c.obs.exit_import_returned.load());
    const StartupChildPpuStartResult again = c.child->poll_exit();
    CHECK(again.status == StartupChildPpuStartStatus::Exited && again.stop == StartupStop::GuestThreadExit);
    r.reap_ok(c);
    CHECK(c.thread.join_calls.load() == 1 && c.thread.join_ok.load() == 1);
    check_released_baseline(r);
    clear_obs();
    r.check_primary_untouched();
}

// ============ 27. a tail-call thunk entry runs its pending real body before the worker returns ============
// The registered entry only sets g_trampoline_fn and returns (like game func_004C9A88); the worker must drain it so the real
// body (sys_ppu_thread_exit through the HLE import) runs. Without the drain the child reports Returned and never enters it.
void case_tail_call_entry() {
    Rig r; ChildRig &c = r.new_child(0);
    StartupChildPpuRequest request = r.request(c, ENTRY_TAIL);
    request.priority = 1001; request.creator_priority = 250; request.stack_size = GD_STACK;
    request.allow_thread_exit = true;
    CHECK(c.child->prepare(r.ports(c), request).status == StartupChildPpuPrepareStatus::Ok);
    c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
    const StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::Exited && s.stop == StartupStop::GuestThreadExit);
    CHECK(c.obs.entered.load() && !c.obs.exit_import_returned.load());
    const StartupChildPpuStatus st = c.child->status();
    CHECK(st.stop == StartupStop::GuestThreadExit && st.terminal_calls == 1);
    r.wait_exited(c);
    r.reap_ok(c);
    check_released_baseline(r);
    clear_obs();
    r.check_primary_untouched();
}

// ============ 10. two independent children ============
void case_two_children() {
    Rig r; const uint32_t qa = r.queue_create(), qb = r.queue_create();
    ChildRig &a = r.new_child(qa); ChildRig &b = r.new_child(qb);
    r.prepare_ok(a); r.prepare_ok(b);
    const StartupChildPpuStatus sa = a.child->status(), sb = b.child->status();
    CHECK(sa.ref.id != sb.ref.id && sa.storage.allocation_address != sb.storage.allocation_address);
    CHECK(sa.storage.allocation_address + sa.storage.allocation_bytes <= sb.storage.allocation_address ||
          sb.storage.allocation_address + sb.storage.allocation_bytes <= sa.storage.allocation_address);   // disjoint
    CHECK(sa.tls.thread_pointer != sb.tls.thread_pointer && r.used() == 0x40000);
    CHECK(a.child->start().status == StartupChildPpuStartStatus::Blocked && b.child->start().status == StartupChildPpuStartStatus::Blocked);
    CHECK(a.obs.r13 == sa.tls.thread_pointer && b.obs.r13 == sb.tls.thread_pointer && a.obs.r1 != b.obs.r1);
    CHECK(be64(a.obs.id_bytes) == sa.ref.id && be64(b.obs.id_bytes) == sb.ref.id);
    r.send(qb, 0xB1, 0xB2, 0xB3);   // wake ONLY B
    r.wait_exited(b);
    CHECK(b.obs.resumed.load() && b.obs.after[2] == 0xB1 && !a.obs.resumed.load());
    CHECK(a.child->status().terminal_calls == 0 && ps3_ppu_service_is_blocked(&r.ppu, sa.ref));   // A untouched, still blocked
    r.reap_ok(b);
    CHECK(r.used() == 0x20000 && a.child->status().has_resources && a.child->status().admitted);   // A's resources survive B's reap
    a.child->cancel(); r.wait_exited(a); r.reap_ok(a);
    CHECK(a.stop_counter.calls.load() >= 1 && b.stop_counter.calls.load() >= 1);
    check_released_baseline(r); r.check_primary_untouched(); clear_obs();
}

// ============ 11. failures BEFORE the reservation leave the object empty and the world untouched ============
void case_fail_before_reservation() {
    Rig r; ChildRig &c = r.new_child(0);
    const StartupChildPpuPorts good = r.ports(c);
    const StartupChildPpuRequest req = r.request(c);
    auto expect_empty = [&](const StartupChildPpuPorts &p, const StartupChildPpuRequest &q, StartupChildPpuPrepareStatus want) {
        const StartupChildPpuPrepareResult x = c.child->prepare(p, q);
        CHECK(x.status == want && !x.owns_resources);
        const StartupChildPpuStatus s = c.child->status();
        CHECK(s.phase == StartupChildPpuPhase::Empty && !s.has_resources && !s.service_owned && !s.admitted && c.child->id() == 0);
        check_released_baseline(r);
        CHECK(c.child->reap().status == StartupChildPpuReapStatus::NothingOwned);
    };
    StartupChildPpuPorts p = good; p.code = nullptr; expect_empty(p, req, StartupChildPpuPrepareStatus::InvalidArgument);
    p = good; p.thread.join = nullptr; expect_empty(p, req, StartupChildPpuPrepareStatus::InvalidArgument);
    p = good; p.dispatch_stop.stop_requested = nullptr; expect_empty(p, req, StartupChildPpuPrepareStatus::InvalidArgument);
    p = good; p.dispatch_limit = 0; expect_empty(p, req, StartupChildPpuPrepareStatus::InvalidArgument);
    p = good; p.recorder = nullptr; expect_empty(p, req, StartupChildPpuPrepareStatus::InvalidArgument);
    StartupChildPpuRequest q = req; q.stack_entry_slack = 0x71; expect_empty(good, q, StartupChildPpuPrepareStatus::InvalidLayout);
    q = req; q.stack_entry_slack = 0x20000; expect_empty(good, q, StartupChildPpuPrepareStatus::InvalidLayout);
    q = req; q.entry_address = 0; expect_empty(good, q, StartupChildPpuPrepareStatus::InvalidLayout);
    q = req; q.tls_template.memory_size = 0xF000; expect_empty(good, q, StartupChildPpuPrepareStatus::InvalidLayout);   // needs 0x17000 > 0xF000
    q = req; q.tls_template.file_size = 0x300; expect_empty(good, q, StartupChildPpuPrepareStatus::InvalidLayout);    // file > mem
    // quota exhaustion is the domain's real refusal: nothing retained
    Rig tiny(0x10000); ChildRig &t = tiny.new_child(0);
    const StartupChildPpuPrepareResult x = t.child->prepare(tiny.ports(t), tiny.request(t));
    CHECK(x.status == StartupChildPpuPrepareStatus::ReserveFailed && x.native_rc == PS3_PPU_ENOMEM && !x.owns_resources);
    CHECK(!t.child->status().has_resources && t.child->status().phase == StartupChildPpuPhase::Empty);
    check_released_baseline(tiny);
    // the first object is still prepare-able after a validation failure
    r.prepare_ok(c); r.reap_ok(c); check_released_baseline(r);
    // single use: no reuse after a complete reap
    CHECK(c.child->prepare(r.ports(c), r.request(c)).status == StartupChildPpuPrepareStatus::NotEmpty);
    clear_obs();
}

// ============ 12. failures AFTER the reservation retain it for an explicit reap ============
void case_fail_after_reservation() {
    {   // TLS template source unreadable: the reservation is owned, no admission was made
        Rig r; ChildRig &c = r.new_child(0);
        StartupChildPpuRequest q = r.request(c); q.tls_template.source = 0x0F000000;
        const StartupChildPpuPrepareResult x = c.child->prepare(r.ports(c), q);
        CHECK(x.status == StartupChildPpuPrepareStatus::TlsFailed && x.owns_resources && x.tls.status == PS3_TLS_MEMORY_FAILURE);
        const StartupChildPpuStatus s = c.child->status();
        CHECK(s.has_resources && s.service_owned && !s.admitted && !s.host_started && s.phase == StartupChildPpuPhase::Reserved && c.child->id() != 0);
        CHECK(r.used() == 0x20000 && c.child->start().status == StartupChildPpuStartStatus::NotReserved);   // never starts unadmitted
        r.reap_ok(c); check_released_baseline(r); clear_obs();
    }
    {   // admission failure (recorder never configured): reservation and TLS stay owned
        Rig r; ChildRig &c = r.new_child(0, false);
        const StartupChildPpuPrepareResult x = c.child->prepare(r.ports(c), r.request(c));
        CHECK(x.status == StartupChildPpuPrepareStatus::AdmissionFailed && x.owns_resources);
        CHECK(c.child->status().has_resources && !c.child->status().admitted && r.used() == 0x20000);
        r.reap_ok(c); check_released_baseline(r); clear_obs();
    }
}

// ============ 13. start-port failure leaves RESERVED, never claims a host start ============
void case_start_failure() {
    Rig r; ChildRig &c = r.new_child(0);
    r.prepare_ok(c);
    c.thread.fail_start = true;
    const StartupChildPpuStartResult s = c.child->start();
    CHECK(s.status == StartupChildPpuStartStatus::StartFailed && s.native_rc == PS3_PPU_EAGAIN);
    const StartupChildPpuStatus st = c.child->status();
    CHECK(st.phase == StartupChildPpuPhase::Reserved && !st.host_started && st.service_owned && st.admitted && c.thread.start_calls.load() == 1);
    r.reap_ok(c);
    CHECK(c.thread.join_calls.load() == 0);   // no host thread to join
    check_released_baseline(r); clear_obs();
}

// ============ 14. host join failure retains everything; retry performs exactly one successful host join ============
void case_join_failure_retry() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c);
    CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
    c.child->cancel(); r.wait_exited(c);
    c.thread.fail_join = 1;
    const StartupChildPpuReapResult first = c.child->reap();
    CHECK(first.status == StartupChildPpuReapStatus::ServiceRetained && first.service_rc == PS3_PPU_PROVIDER_FAILURE);
    StartupChildPpuStatus s = c.child->status();
    CHECK(s.has_resources && s.service_owned && s.admitted && s.phase == StartupChildPpuPhase::Started && r.used() == 0x20000);
    CHECK(c.thread.join_ok.load() == 0);
    r.reap_ok(c);
    CHECK(c.thread.join_calls.load() == 2 && c.thread.join_ok.load() == 1);
    check_released_baseline(r); r.check_primary_untouched(); clear_obs();
}

// ============ 15. decommit failure: host join completed once and remembered; retry frees only; admission kept until then ============
void case_decommit_failure_retry() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c);
    const StartupChildPpuStatus pre = c.child->status();
    CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
    c.child->cancel(); r.wait_exited(c);
    r.spy.fail_decommit.store(1);
    const StartupChildPpuReapResult first = c.child->reap();
    CHECK(first.status == StartupChildPpuReapStatus::ServiceRetained && first.service_rc == PS3_PPU_PROVIDER_FAILURE);
    StartupChildPpuStatus s = c.child->status();
    CHECK(s.has_resources && s.service_owned && s.admitted && r.used() == 0x20000 && r.allocations() == 1);
    CHECK(r.all(pre.storage.stack_address, 8, FILL));   // still mapped and owned
    CHECK(c.thread.join_calls.load() == 1 && c.thread.join_ok.load() == 1);
    r.reap_ok(c);
    CHECK(c.thread.join_calls.load() == 1 && c.thread.join_ok.load() == 1);   // retry did NOT join the host thread again
    CHECK(r.spy.decommit_ok.load() >= 1);
    check_released_baseline(r); r.check_primary_untouched(); clear_obs();
    CHECK(c.child->reap().status == StartupChildPpuReapStatus::NothingOwned && c.thread.join_calls.load() == 1);
}

// ============ 16. P2-2: cancelled child still RUNNING compiled code: reap is StillRunning and never joins ============
void case_cancel_running_reap_still_running() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c, ENTRY_GATED);
    CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
    r.send(q, 0x61, 0x62, 0x63);   // real wake: the body leaves its receive and runs compiled code up to the release latch
    arm(10000); while (!c.obs.gate_reached.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!r.exited(c) && !ps3_ppu_service_is_blocked(&r.ppu, c.child->status().ref) && c.obs.resumed.load());
    c.child->cancel();
    CHECK(c.child->status().cancel_requested);
    for (int i = 0; i < 3; ++i) {   // repeated attempts stay StillRunning; the host thread is never joined while the activity runs
        const StartupChildPpuReapResult x = c.child->reap();
        CHECK(x.status == StartupChildPpuReapStatus::StillRunning && c.thread.join_calls.load() == 0);
        const StartupChildPpuStatus st = c.child->status();
        CHECK(st.phase == StartupChildPpuPhase::Started && st.has_resources && st.service_owned && st.admitted && r.used() == 0x20000);
    }
    c.obs.gate_open.store(true);   // release into the real cancellation boundary (the next sc130 receive)
    r.wait_exited(c);
    const StartupChildPpuStatus done = c.child->status();
    CHECK(!c.obs.gate_timeout.load() && !c.obs.second_returned.load() && done.stop == StartupStop::Cancelled && done.terminal_calls == 1);
    CHECK(done.consumed_events == 1 && done.first_consumed.data1 == 0x61);
    CHECK(c.thread.join_calls.load() == 0);
    r.reap_ok(c);
    CHECK(c.thread.join_calls.load() == 1 && c.thread.join_ok.load() == 1);   // exactly one host join
    CHECK(c.child->reap().status == StartupChildPpuReapStatus::NothingOwned && c.thread.join_calls.load() == 1);
    check_released_baseline(r); r.check_primary_untouched(); clear_obs();
}

// ============ 17. P2-3: an event consumed by the provider is recorded even when the bridge stops AFTER the provider returned ============
// Seam: the PER-CHILD budget clock (an existing injected port). The bridge polls the budget after the callback returned and before it
// publishes r4..r7, so exhausting the budget while the child is blocked reproduces "dequeued, then bridge stop" deterministically.
// (A cancellation variant has no seam without a production hook, so the same bridge boundary is exercised through the budget.)
void case_consumed_then_budget_stop() {
    Rig r; const uint32_t q = r.queue_create();
    ChildRig &c = r.new_child(q);
    r.prepare_ok(c);
    CHECK(c.child->start().status == StartupChildPpuStartStatus::Blocked);
    CHECK(c.child->status().consumed_events == 0);
    c.budget_time.store(UINT64_C(1) << 50);
    r.send(q, 0x51, 0x52, 0x53);
    r.wait_exited(c);
    const StartupChildPpuStatus s = c.child->status();
    std::printf("consumed_then_budget_stop: stop=%d consumed=%u fault=%d discarded=%d\n", int(s.stop), s.consumed_events, int(s.fault), int(s.discarded_event));
    CHECK(!c.obs.resumed.load());   // r3..r7 were never published into the guest
    CHECK(s.stop != StartupStop::Returned && s.stop != StartupStop::None && s.terminal_calls == 1);
    CHECK(!s.cancel_requested && !s.discarded_event && s.fault == StartupChildPpuReceiveFault::None && s.last_native_rc == PS3_LV2_OK);
    CHECK(s.receive_calls == 1);   // no later receive
    CHECK(s.consumed_events == 1 && s.first_consumed.data1 == 0x51 && s.first_consumed.data2 == 0x52 && s.first_consumed.data3 == 0x53);
    ps3_lv2_queue snap; CHECK(ps3_lv2_queue_snapshot(&r.lv2, q, &snap) == PS3_LV2_OK && snap.count == 0);   // really consumed
    r.reap_ok(c); check_released_baseline(r); r.check_primary_untouched(); clear_obs();
}

void case_boot_content_copy() {
    for (BootContentOrigin origin : {BootContentOrigin::Unavailable,BootContentOrigin::DiscPackage,BootContentOrigin::InstalledPackage}) {
        clear_obs(); Rig r; const uint32_t queue=r.queue_create(); ChildRig &c=r.new_child(queue);
        StartupChildPpuRequest request=r.request(c);
        const BootContentMetadata expected=boot_content_fixture(origin); request.boot_content=expected;
        const auto prepared=c.child->prepare(r.ports(c),request);
        CHECK(prepared.status==StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id=c.child->id(); g_obs[0]=&c.obs; r.write_id(c);
        request.boot_content=boot_content_fixture(BootContentOrigin::InstalledPackage);
        request.boot_content.title_id[0]='X'; request.boot_content.parental_level=99;
        CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
        CHECK(c.obs.execution_control!=r.primary_control.get());
        CHECK(same_boot_content(c.obs.boot_content_before,expected));
        CHECK(!c.obs.boot_disc_calls && !c.obs.boot_disc_returns && c.obs.boot_disc_observation_fresh);
        r.primary.boot_content=request.boot_content;
        r.send(queue,1,2,3); r.wait_exited(c); r.reap_ok(c);
        CHECK(c.obs.resumed.load() && same_boot_content(c.obs.boot_content_after,expected));
        r.check_primary_untouched(); check_released_baseline(r); clear_obs();
    }
}
bool same_system_parameters(const SystemParameters &a,const SystemParameters &b) {
    return a.language.status==b.language.status && a.language.language==b.language.language &&
        a.language_compatibility==b.language_compatibility && a.profile==b.profile &&
        a.confirm_button==b.confirm_button && a.date_format==b.date_format &&
        a.time_format==b.time_format && a.parental_level==b.parental_level;
}
SystemParameters system_parameters_fixture(SystemLanguageStatus status=SystemLanguageStatus::Available) {
    SystemParameters p{};
    p.language={status,SystemLanguage::EnglishGB};
    p.language_compatibility=SystemLanguageCompatibility::NativePortLegacyFamilies;
    p.profile=SystemParameterProfile::NativePortConventions;
    p.confirm_button=SystemConfirmButton::Circle; p.date_format=SystemDateFormat::DayMonthYear;
    p.time_format=SystemTimeFormat::TwelveHour; p.parental_level=9;
    return p;
}
void case_system_parameters_copy() {
    for(auto status:{SystemLanguageStatus::Available,SystemLanguageStatus::Unavailable,SystemLanguageStatus::Unsupported}) {
        clear_obs(); Rig r; const uint32_t queue=r.queue_create(); ChildRig &c=r.new_child(queue);
        StartupChildPpuRequest request=r.request(c);
        const auto expected=system_parameters_fixture(status); request.system_parameters=expected;
        CHECK(c.child->prepare(r.ports(c),request).status==StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id=c.child->id(); g_obs[0]=&c.obs; r.write_id(c);
        request.system_parameters={};
        CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
        CHECK(c.obs.execution_control!=r.primary_control.get());
        CHECK(same_system_parameters(c.obs.system_parameters_before,expected));
        CHECK(!c.obs.system_parameter_calls && !c.obs.system_parameter_returns && c.obs.system_parameter_observation_fresh);
        r.primary.system_parameters={};
        r.send(queue,1,2,3); r.wait_exited(c); r.reap_ok(c);
        CHECK(c.obs.resumed.load() && same_system_parameters(c.obs.system_parameters_after,expected));
        r.check_primary_untouched(); check_released_baseline(r); clear_obs();
    }
}
void case_system_sdk_copy() {
    for(auto language:{SystemLanguage::EnglishGB,SystemLanguage::PortugueseBR}) for(bool valid:{true,false}) {
        clear_obs(); Rig r; const uint32_t queue=r.queue_create(); ChildRig &c=r.new_child(queue);
        c.obs.query_language=true;
        auto request=r.request(c); request.system_parameters=system_parameters_fixture();
        request.system_parameters.language.language=language;
        request.process_sdk={0x30ffff,valid};
        CHECK(c.child->prepare(r.ports(c),request).status==StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id=c.child->id(); g_obs[0]=&c.obs; r.write_id(c);
        request.process_sdk={0x450000,true};
        CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
        const uint32_t expected=valid?(language==SystemLanguage::EnglishGB?1:7):uint32_t(language);
        CHECK(c.obs.language_effective_before==(valid?0x2101u:0x111u));
        CHECK(c.obs.language_value_before==expected);
        CHECK(c.obs.sdk_before.raw==0x30ffff && c.obs.sdk_before.valid==valid);
        CHECK(c.obs.system_parameter_observation_fresh && !c.obs.system_parameter_calls && !c.obs.system_parameter_returns);
        r.primary.process_sdk={0x450000,true};
        r.send(queue,1,2,3); r.wait_exited(c); r.reap_ok(c);
        CHECK(c.obs.language_effective_after==(valid?0x2101u:0x111u));
        CHECK(c.obs.language_value_after==expected);
        CHECK(c.obs.sdk_after.raw==0x30ffff && c.obs.sdk_after.valid==valid);
        r.check_primary_untouched(); check_released_baseline(r); clear_obs();
    }
}
bool same_system_profile(const SystemProfile &a,const SystemProfile &b) {
    return a.source==b.source &&
        !std::memcmp(a.current_user_display_name,b.current_user_display_name,sizeof a.current_user_display_name) &&
        !std::memcmp(a.device_display_label,b.device_display_label,sizeof a.device_display_label);
}
SystemProfile system_profile_fixture(SystemProfileSource source=SystemProfileSource::NativeApplicationProfile) {
    SystemProfile profile{}; profile.source=source;
    std::memcpy(profile.current_user_display_name,"Player 7",9);
    std::memcpy(profile.device_display_label,"Nintendo Switch portable",25);
    profile.current_user_display_name[63]=static_cast<char>(0xff);
    profile.device_display_label[127]=static_cast<char>(0xfe);
    return profile;
}
void case_system_profile_copy() {
    for(auto source:{SystemProfileSource::Unconfigured,SystemProfileSource::NativeApplicationProfile,static_cast<SystemProfileSource>(99)}) {
        clear_obs(); Rig r; const uint32_t queue=r.queue_create(); ChildRig &c=r.new_child(queue);
        auto request=r.request(c); const auto expected=system_profile_fixture(source); request.system_profile=expected;
        CHECK(c.child->prepare(r.ports(c),request).status==StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id=c.child->id(); g_obs[0]=&c.obs; r.write_id(c);
        request.system_profile={};
        CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
        CHECK(c.obs.execution_control!=r.primary_control.get());
        CHECK(same_system_profile(c.obs.system_profile_before,expected));
        CHECK(!c.obs.system_string_calls && !c.obs.system_string_returns && c.obs.system_string_observation_fresh);
        r.primary.system_profile={};
        r.send(queue,1,2,3); r.wait_exited(c); r.reap_ok(c);
        CHECK(c.obs.resumed.load() && same_system_profile(c.obs.system_profile_after,expected));
        r.check_primary_untouched(); check_released_baseline(r); clear_obs();
    }
}
void case_presentation_topology_copy() {
    for(auto expected:{PresentationTopology::Unconfigured,PresentationTopology::SingleNativeTarget,static_cast<PresentationTopology>(99)}) {
        clear_obs();Rig r;const auto queue=r.queue_create();auto &c=r.new_child(queue);
        c.obs.query_video_count=expected==PresentationTopology::SingleNativeTarget;
        auto request=r.request(c);request.presentation_topology=expected;
        CHECK(c.child->prepare(r.ports(c),request).status==StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id=c.child->id();g_obs[0]=&c.obs;r.write_id(c);
        request.presentation_topology=PresentationTopology::Unconfigured;
        CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
        CHECK(c.obs.topology_before==expected && c.obs.video_observation_fresh);
        CHECK(!c.obs.video_calls_before && !c.obs.video_returns_before);
        r.primary.presentation_topology=PresentationTopology::Unconfigured;
        r.send(queue,1,2,3);r.wait_exited(c);r.reap_ok(c);
        CHECK(c.obs.resumed.load() && c.obs.topology_after==expected);
        if(c.obs.query_video_count) {
            CHECK(c.obs.video_count_before==1 && c.obs.video_count_after==1);
            CHECK(c.obs.video_calls_after==2 && c.obs.video_returns_after==2);
        }
        CHECK(!r.primary_control->video_output_count_calls && !r.primary_control->video_output_count_returns);
        r.check_primary_untouched();check_released_baseline(r);clear_obs();
    }
}
struct DiscRegistryFixture {
    ps3_disc_change_registry registry{};
    ps3_pthread_guard guard{};
    DiscRegistryFixture() {
        CHECK(!ps3_pthread_guard_init(&guard));
        CHECK(ps3_disc_change_init(&registry,ps3_pthread_guard_port(&guard),PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_OK);
    }
    ~DiscRegistryFixture() { CHECK(!ps3_pthread_guard_destroy(&guard)); }
    DiscRegistryFixture(const DiscRegistryFixture &)=delete;
    DiscRegistryFixture &operator=(const DiscRegistryFixture &)=delete;
    ps3_disc_change_pair snapshot() {
        ps3_disc_change_pair pair{};CHECK(ps3_disc_change_read(&registry,&pair)==PS3_DISC_CHANGE_OK);return pair;
    }
    void replace(ps3_disc_change_pair pair) {
        const auto result=ps3_disc_change_register(&registry,pair);
        CHECK(result.status==PS3_DISC_CHANGE_OK && result.committed && same_disc_pair(result.current,pair));
    }
};
void case_disc_change_shared_registry() {
    DiscRegistryFixture shared,alternate;const ps3_disc_change_pair initial{1,3},other{5,7};
    shared.replace(initial);alternate.replace(other);
    clear_obs();Rig r;const auto queue=r.queue_create();auto &c=r.new_child(queue);c.obs.use_disc_hle=true;
    auto request=r.request(c);request.disc_change_callbacks=&shared.registry;
    CHECK(c.child->prepare(r.ports(c),request).status==StartupChildPpuPrepareStatus::Ok);
    c.obs.thread_id=c.child->id();g_obs[0]=&c.obs;r.write_id(c);
    request.disc_change_callbacks=&alternate.registry;
    CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
    CHECK(c.obs.disc_registry_before==&shared.registry && same_disc_pair(c.obs.disc_pair_before,initial));
    CHECK(c.obs.disc_observation_fresh && !c.obs.disc_calls_before && !c.obs.disc_returns_before);
    CHECK(same_disc_pair(c.obs.disc_registration_before.registration.previous,initial));
    CHECK(same_disc_pair(shared.snapshot(),DISC_ENTRY));
    shared.replace(DISC_PARENT);r.primary.disc_change_callbacks=&alternate.registry;
    r.send(queue,1,2,3);r.wait_exited(c);r.reap_ok(c);
    CHECK(c.obs.disc_registry_after==&shared.registry && same_disc_pair(c.obs.disc_pair_after,DISC_PARENT));
    CHECK(same_disc_pair(c.obs.disc_registration_after.registration.previous,DISC_PARENT));
    CHECK(c.obs.disc_calls_after==2 && c.obs.disc_returns_after==2);
    CHECK(same_disc_pair(shared.snapshot(),DISC_RESUME) && same_disc_pair(alternate.snapshot(),other));
    CHECK(!r.primary_control->disc_change_calls && !r.primary_control->disc_change_returns);
    r.check_primary_untouched();check_released_baseline(r);clear_obs();
}
void case_presentation_mode_copy() {
    for(auto expected:{NativePresentationMode::Unconfigured,NativePresentationMode::Fixed720p,static_cast<NativePresentationMode>(99)}) {
        clear_obs();Rig r;const auto queue=r.queue_create();auto &c=r.new_child(queue);
        auto request=r.request(c);request.presentation_mode=expected;
        CHECK(c.child->prepare(r.ports(c),request).status==StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id=c.child->id();g_obs[0]=&c.obs;r.write_id(c);
        request.presentation_mode=NativePresentationMode::Unconfigured;
        CHECK(c.child->start().status==StartupChildPpuStartStatus::Blocked);
        CHECK(c.obs.mode_before==expected);
        r.primary.presentation_mode=NativePresentationMode::Unconfigured;
        r.send(queue,1,2,3);r.wait_exited(c);r.reap_ok(c);
        CHECK(c.obs.resumed.load() && c.obs.mode_after==expected);
        r.check_primary_untouched();check_released_baseline(r);clear_obs();
    }
}
// ============ 26. decision 14: a child with process_services reaches the process's allocate_memory through syscall 348 ============
// The child's compiled body issues the real lv2 sc348 route. With ports.process_services set, prepare() copies the process's
// allocate/free/query capabilities and their context into the child's own services, so the process pool really allocates. Without it,
// the same syscall stops MissingServices exactly as before (the previous behaviour is preserved bit for bit).
void case_process_services_allocate() {
    {   // borrowed: the real allocation happens in the PROCESS pool, charged to it, and the child returns 0
        Rig r; ChildRig &c = r.new_child(0);
        const uint64_t used_before = r.used();
        StartupChildPpuPorts p = r.ports(c); p.process_services = &r.process_services;
        const StartupChildPpuPrepareResult prep = c.child->prepare(p, r.request(c, ENTRY_ALLOC));
        CHECK(prep.status == StartupChildPpuPrepareStatus::Ok && prep.owns_resources);
        c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
        const StartupChildPpuStartResult s = c.child->start();
        CHECK(s.status == StartupChildPpuStartStatus::Returned && s.stop == StartupStop::Returned);
        r.wait_exited(c);
        CHECK(c.obs.entered.load());
        CHECK(r.process_alloc_calls.load() == 1);                              // the PROCESS provider really ran
        CHECK(c.obs.alloc_r3 == 0 && c.obs.alloc_service_status == PS3_USER_MEMORY_SERVICE_OK);   // CELL_OK from the real service
        CHECK(c.obs.alloc_published != 0 && c.obs.alloc_published % 0x10000 == 0);                // a real 64 KiB-aligned guest address
        const StartupChildPpuStatus st = c.child->status();
        CHECK(c.child->control().syscall_number == 348 && c.child->control().memory_result.status == PS3_USER_MEMORY_SERVICE_OK);
        CHECK(st.stop == StartupStop::Returned && st.terminal_calls == 1);
        CHECK(r.used() > used_before);                                          // the process pool grew
        r.reap_ok(c);
        // The child's OWN admission/storage are gone; the PROCESS allocation is the process's (not the child's), so it stays charged
        // until the process frees it - exactly the sharing decision 14 describes.
        CHECK(r.used() == used_before + ALLOC_SIZE && ps3_ppu_threads_live(&r.ppu) == 0);
        CHECK(ps3_user_memory_free(&r.memory, uint32_t(c.obs.alloc_published)).status == PS3_USER_MEMORY_OK);
        CHECK(r.used() == used_before && r.allocations() == 0);
        clear_obs();
    }
    {   // null process_services: today's behaviour is unchanged - MissingServices, no process allocation, no child terminal memory result
        Rig r; ChildRig &c = r.new_child(0);
        r.prepare_ok(c, ENTRY_ALLOC);
        const StartupChildPpuStartResult s = c.child->start();
        CHECK(s.status == StartupChildPpuStartStatus::Terminal && s.stop == StartupStop::MissingServices);
        r.wait_exited(c);
        CHECK(r.process_alloc_calls.load() == 0);
        CHECK(c.child->control().syscall_number == 348);
        // the service was never invoked: the zeroed memory_result is the untouched default, so assert the provider was not reached
        CHECK(c.child->control().memory_result.retained_allocation == 0 && c.child->control().memory_result.published == 0);
        r.reap_ok(c); check_released_baseline(r); clear_obs();
    }
}

// ============ 27. decision 14: a child with fixed_timezone reaches syscall 144; without it the syscall stops MissingServices ============
// The sc144 route exists only with PS3_NATIVE_STARTUP_TIMEZONE (native/Makefile N12_STARTUP_FEATURES). This verifier does not set it, so
// the case is feature-aware: with the flag it proves the borrowed timezone really reaches the child; without it, it pins the documented
// stop and proves that a child NEVER invents a timezone. Both arms are real, and neither invents a capability.
void case_process_fixed_timezone() {
#ifdef PS3_NATIVE_STARTUP_TIMEZONE
    {   // borrowed fixed timezone: the guest outputs are written from the process's values and the syscall returns
        Rig r; ChildRig &c = r.new_child(0);
        StartupChildPpuPorts p = r.ports(c); p.fixed_timezone = &r.process_timezone; p.process_services = &r.process_services;
        CHECK(c.child->prepare(p, r.request(c, ENTRY_TZ)).status == StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
        const StartupChildPpuStartResult s = c.child->start();
        CHECK(s.status == StartupChildPpuStartStatus::Returned && s.stop == StartupStop::Returned);
        r.wait_exited(c);
        CHECK(c.obs.entered.load());
        CHECK(c.obs.tz_r3 == 0);                                     // CELL_OK
        CHECK(c.obs.tz_written_minutes == uint64_t(int64_t(TZ_MINUTES)));
        CHECK(c.obs.tz_written_summertime == uint64_t(int64_t(TZ_SUMMER_MINUTES)));
        const StartupControl &k = c.child->control();
        CHECK(k.syscall_number == 144 && k.timezone_calls == 1);
        CHECK(k.timezone_minutes == TZ_MINUTES && k.summertime_minutes == TZ_SUMMER_MINUTES);
        r.reap_ok(c); check_released_baseline(r); clear_obs();
    }
    {   // null fixed_timezone: MissingServices, no guest write
        Rig r; ChildRig &c = r.new_child(0);
        StartupChildPpuPorts p = r.ports(c); p.process_services = &r.process_services;
        CHECK(c.child->prepare(p, r.request(c, ENTRY_TZ)).status == StartupChildPpuPrepareStatus::Ok);
        c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
        const StartupChildPpuStartResult s = c.child->start();
        CHECK(s.status == StartupChildPpuStartStatus::Terminal && s.stop == StartupStop::MissingServices);
        r.wait_exited(c);
        const StartupControl &k = c.child->control();
        CHECK(k.syscall_number == 144 && k.timezone_calls == 0);
        CHECK(c.obs.tz_r3 == 0 && c.obs.tz_written_minutes == 0 && c.obs.tz_written_summertime == 0);
        r.reap_ok(c); check_released_baseline(r); clear_obs();
    }
#else
    // The sc144 dispatch is compiled out of this build: a child that borrows the process timezone still gets no timezone (the syscall
    // stops before any guest write), and no child ever invents one. This is the same honest stop the target shows when the feature is off.
    Rig r; ChildRig &c = r.new_child(0);
    StartupChildPpuPorts p = r.ports(c); p.fixed_timezone = &r.process_timezone; p.process_services = &r.process_services;
    CHECK(c.child->prepare(p, r.request(c, ENTRY_TZ)).status == StartupChildPpuPrepareStatus::Ok);
    c.obs.thread_id = c.child->id(); g_obs[0] = &c.obs; r.write_id(c);
    const StartupChildPpuStartResult s = c.child->start();
    // With the sc144 dispatch compiled out, the syscall is refused as an unsupported lv2 number AFTER the service check passed
    // (process_services is present): the child reaches the memory/capability gate and the exact syscall is not served.
    CHECK(s.status == StartupChildPpuStartStatus::Terminal && s.stop == StartupStop::UnsupportedSyscall);
    r.wait_exited(c);
    const StartupControl &k = c.child->control();
    CHECK(k.syscall_number == 144 && k.timezone_calls == 0);
    CHECK(c.obs.tz_r3 == 0 && c.obs.tz_written_minutes == 0 && c.obs.tz_written_summertime == 0);
    r.reap_ok(c); check_released_baseline(r); clear_obs();
#endif
}

struct Case { const char *name; void (*run)(); };
const Case cases[] = {
    {"presentation_topology_copy",case_presentation_topology_copy},
    {"presentation_mode_copy",case_presentation_mode_copy},
    {"disc_change_shared_registry",case_disc_change_shared_registry},
    {"system_profile_copy",case_system_profile_copy},
    {"system_sdk_copy",case_system_sdk_copy},
    {"system_parameters_copy",case_system_parameters_copy},
    {"boot_content_copy",case_boot_content_copy},
    {"prepare_layout_and_cpu", case_prepare_layout_and_cpu}, {"block_then_resume", case_block_then_resume},
    {"id_not_invented", case_id_not_invented}, {"cancel_blocked", case_cancel_blocked},
    {"cancel_before_start", case_cancel_before_start}, {"race_event_vs_cancel", case_race_event_vs_cancel},
    {"receive_faults", case_receive_faults}, {"unknown_entry", case_unknown_entry}, {"returns", case_returns},
    {"two_children", case_two_children}, {"fail_before_reservation", case_fail_before_reservation},
    {"fail_after_reservation", case_fail_after_reservation}, {"start_failure", case_start_failure},
    {"join_failure_retry", case_join_failure_retry}, {"decommit_failure_retry", case_decommit_failure_retry},
    {"cancel_running_reap_still_running", case_cancel_running_reap_still_running}, {"consumed_then_budget_stop", case_consumed_then_budget_stop},
    {"thread_exit_allowed", case_thread_exit_allowed}, {"lower_priority_not_fenced", case_lower_priority_not_fenced},
    {"background_start", case_background_start}, {"tail_call_entry", case_tail_call_entry},
    {"process_services_allocate", case_process_services_allocate}, {"process_fixed_timezone", case_process_fixed_timezone}};
}

// --isolated: every case in its own process (so a RED run reports ALL failing cases, not only the first _exit).
int run_isolated(char **argv, size_t n)
{
    unsigned failed = 0;
    for (size_t i = 0; i < n; ++i) {
        std::fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) { execl("/proc/self/exe", argv[0], cases[i].name, static_cast<char *>(nullptr)); _exit(127); }
        int status = 0;
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::printf("ISOLATED %s %s (status=0x%x)\n", cases[i].name, ok ? "PASS" : "FAIL", unsigned(status)); std::fflush(stdout);
        if (!ok) ++failed;
    }
    std::printf("ISOLATED_SUMMARY failed=%u of %zu\n", failed, n);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    const size_t n = sizeof cases / sizeof *cases;
    if (argc > 1 && !strcmp(argv[1], "--isolated")) return run_isolated(argv, n);
    std::thread(watchdog).detach();
    if (argc > 1 && !strcmp(argv[1], "--list")) { for (size_t i = 0; i < n; ++i) std::printf("%s\n", cases[i].name); return 0; }
    for (size_t i = 0; i < n; ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        arm(60000);
        std::printf("CASE %s\n", cases[i].name); std::fflush(stdout);
        cases[i].run();
        std::printf("PASS %s\n", cases[i].name); std::fflush(stdout);
    }
    deadline_ms.store(0);
    std::printf("CHILD_PPU_ALL_PASS\n");
    return 0;
}

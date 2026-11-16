#include "../adapters/startup_execution.cpp"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>
#include <initializer_list>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("SPURS_ATTRIBUTES_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
pthread_mutex_t *terminal_guard;
void lock(void *p) { if (pthread_mutex_lock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
void unlock(void *p) { if (pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
// Translate the high guest window into the bounded host test backing. All
// coherence, reservations, permissions and admission remain production code.
struct GuestWindow {
    static constexpr uint32_t base = 0xd00ff000;
    ElfTestHostSpace *space = elf_test_host_space_create(0x3000);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    StartupControl *control = nullptr;
    size_t reads = 0, writes = 0;
    bool cancel_read = false, cancel_write = false, fail_read = false, fail_write = false;
    uint32_t cancel_write_address = 0, fail_write_address = 0, partial_write_address = 0;
    bool fail_clock_after_write = false, clock_triggered = false;
    uint32_t clock_trigger_address = 0;
    static bool contains(uint32_t a, uint64_t n) {
        return a >= base && uint64_t(a) - base <= 0x3000 && n <= 0x3000 - (uint64_t(a) - base);
    }
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
        if (s.fail_read) return PS3_GUEST_MEMORY_UNMAPPED;
        const auto status = contains(a, n) ? s.backing.read(s.backing.context, a - base, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
        if (s.cancel_read) s.control->cancelled.store(true);
        return status;
    }
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *b, size_t n) {
        auto &s = *static_cast<GuestWindow *>(p); ++s.writes;
        if (s.fail_write || a == s.fail_write_address) return PS3_GUEST_MEMORY_UNMAPPED;
        if (a == s.partial_write_address) {
            CHECK(contains(a,n) && n >= 2);
            CHECK(s.backing.write(s.backing.context,a-base,b,2) == PS3_GUEST_MEMORY_OK);
            return PS3_GUEST_MEMORY_UNMAPPED;
        }
        const auto status = contains(a, n) ? s.backing.write(s.backing.context, a - base, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
        if (s.cancel_write || a == s.cancel_write_address) s.control->cancelled.store(true);
        if (a == s.clock_trigger_address) s.clock_triggered = true;
        return status;
    }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.zero(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
    ~GuestWindow() { elf_test_host_space_destroy(space); }
};
struct Runtime {
    GuestWindow window;
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[2]{};
    ps3_atomic_pin_record pins[2]{};
    PpuMemoryRamRange ram{GuestWindow::base, 8192, true};
    PpuMemoryRamRange partial_permissions[3];
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[64]{};
    unsigned clock_calls = 0, fail_clock_at = 0;
    static ps3_clock_status sample(void *p, uint64_t *value) {
        auto &s = *static_cast<Runtime *>(p);
        if (++s.clock_calls == s.fail_clock_at || (s.window.fail_clock_after_write && s.window.clock_triggered)) return PS3_CLOCK_ARGUMENT;
        *value = 1; return PS3_CLOCK_OK;
    }
    ps3_clock clock{this, sample};
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    ps3_atomic_domain &initialize() {
        if (!window.space || ps3_atomic_domain_init(&domain, window.port(), {&guard, lock, unlock}, owners, 2, pins, 2) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    Runtime(uint64_t hle_limit = 64, int32_t guest_priority = 250, bool is_writable = true,
        uint64_t permitted_length = 8192, bool has_guest_priority = true, uint32_t thread_id = 1, size_t trace_capacity = 64, bool readonly_attribute = false)
        : ram{GuestWindow::base, permitted_length, is_writable},
          partial_permissions{{GuestWindow::base,0x990,is_writable},{GuestWindow::base+0x990,512,false},
              {GuestWindow::base+0xb90,permitted_length-0xb90,is_writable}},
          binding(initialize(), readonly_attribute?partial_permissions:&ram, readonly_attribute?3:1),
        policy({64, hle_limit, 64, UINT64_C(1000000000)}, 0), budget(policy, clock) {
        CHECK(ps3_atomic_commit(&domain, ram.address, 8192) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        execution.control = control.get(); window.control = control.get();
        execution.guest_priority = guest_priority; execution.has_guest_priority = has_guest_priority;
        execution.ppu.cpu.thread_id = thread_id;
        execution.ppu.cpu.gpr[1] = 0xd00ff910;
        execution.ppu.cpu.gpr[2] = UINT64_C(0x123456789abcdef0);
        execution.ppu.cpu.lr = 0x2cfe70;
        CHECK(recorder.configure(events, trace_capacity, PpuMemoryTraceRetention::Strict, 0));
        CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
        unsigned char bytes[8192]; std::memset(bytes, 0xa5, sizeof(bytes));
        CHECK(binding.coherent_memory.write(binding.coherent_memory.context, ram.address, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK);
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(!pthread_mutex_destroy(&guard));
    }
    struct Range { uint32_t address; size_t length; };
    unsigned char before_memory[8192]{}, after_memory[8192]{};
    size_t event_begin = 0, event_end = 0;
    void seed(uint32_t address, const void *bytes, size_t length) {
        CHECK(binding.coherent_memory.write(binding.coherent_memory.context,address,bytes,length) == PS3_GUEST_MEMORY_OK);
    }
    void seed_word(uint32_t address, uint32_t value) {
        const unsigned char encoded[]{static_cast<unsigned char>(value>>24),static_cast<unsigned char>(value>>16),static_cast<unsigned char>(value>>8),static_cast<unsigned char>(value)};
        seed(address,encoded,4);
    }
    uint32_t word(uint32_t address) {
        unsigned char bytes[4]{}; CHECK(window.backing.read(window.backing.context,address-GuestWindow::base,bytes,4) == PS3_GUEST_MEMORY_OK);
        return uint32_t(bytes[0])<<24 | uint32_t(bytes[1])<<16 | uint32_t(bytes[2])<<8 | uint32_t(bytes[3]);
    }
    void call(uint32_t nid, std::initializer_list<uint64_t> arguments,
        std::initializer_list<Range> outputs, uint64_t result = 0) {
        auto &cpu = execution.ppu.cpu;
        for (unsigned i = 3; i <= 9; ++i) cpu.gpr[i] = 0;
        unsigned reg = 3; for (uint64_t value : arguments) { CHECK(reg <= 9); if (reg <= 9) cpu.gpr[reg++] = value; }
        const auto before = cpu;
        event_begin = recorder.report().published_count;
        const auto counts = policy.snapshot(); const bool cancelled = control->cancelled.load();
        const auto imports = control->import_count;
        CHECK(window.backing.read(window.backing.context, 0, before_memory, sizeof(before_memory)) == PS3_GUEST_MEMORY_OK);
        control->stop = StartupStop::None;
        active = control.get(); control->cpu = &cpu; terminal_guard = &guard;
        try { ps3_hle_call(nid, &cpu); active = nullptr; } catch (const Terminal &) {}
        auto expected = before; if (control->stop == StartupStop::None) expected.gpr[3] = result;
        CHECK(!std::memcmp(&expected, &cpu, sizeof(cpu)));
        CHECK(control->journal.count == 0 && !recorder.report().batch_active && !recorder.report().remaining_publications);
        const bool admitted = !cancelled && counts.hle < policy.limits().hle;
        CHECK(control->import_count == imports + (admitted ? 1 : 0));
        if (admitted) CHECK(control->imports[imports] == nid);
        CHECK(policy.snapshot().hle == counts.hle + (admitted ? 1 : 0));
        CHECK(policy.snapshot().generated == counts.generated && policy.snapshot().indirect == counts.indirect);
        CHECK(window.backing.read(window.backing.context, 0, after_memory, sizeof(after_memory)) == PS3_GUEST_MEMORY_OK);
        event_end = recorder.report().published_count;
        for (size_t event_index = event_begin; event_index < event_end; ++event_index) {
            const auto &event = events[event_index];
            CHECK(event.origin == PpuMemoryTraceOrigin::Hle && event.operation == PpuMemoryTraceOperation::Write && event.status == PS3_GUEST_MEMORY_OK);
            bool permitted = false;
            for (const auto &range : outputs) if (event.address >= range.address && uint64_t(event.address)-range.address <= range.length && event.length <= range.length-(uint64_t(event.address)-range.address)) permitted = true;
            CHECK(permitted && event.length > 0);
            if (!permitted) continue;
            const auto *actual = after_memory+(event.address-GuestWindow::base);
            if (event.byte_transfer) {
                CHECK(event.length <= PPU_MEMORY_TRACE_BYTE_CAPACITY);
                CHECK(!std::memcmp(actual,event.bytes,event.length));
            } else {
                CHECK(event.length == 4 || event.length == 8);
                uint64_t value = 0; for(size_t i=0;i<event.length;++i) value=(value<<8)|actual[i];
                CHECK(value == event.value);
            }
        }
        for (size_t i = 0; i < sizeof(before_memory); ++i) {
            const uint32_t address = GuestWindow::base + uint32_t(i); bool permitted = false;
            for (const auto &range : outputs) if (address >= range.address && uint64_t(address) - range.address < range.length) permitted = true;
            if (!permitted) CHECK(before_memory[i] == after_memory[i]);
        }
    }
    void actual_request() {
        CHECK(!control->deadline_tick && !control->cancelled.load());
        call(0x95180230, {0xd00ff990, 2, 0x250001, 5, 128, 249, 0}, {{0xd00ff990,512},{0xd00ff938,8}});
#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
        CHECK(control->stop == StartupStop::None);
        unsigned char expected[512]{};
        const unsigned char first[]{0,0,0,2, 0,0x25,0,1, 0,0,0,5, 0,0,0,128, 0,0,0,249};
        std::memcpy(expected,first,sizeof(first));
        CHECK(!std::memcmp(after_memory+0x990,expected,sizeof(expected)));
        const auto &captured=control->spurs_attribute;
        CHECK(captured.has_result && captured.result==0 && captured.attribute_bytes==512 && captured.last_attribute_address==0xd00ff990 && captured.last_attribute_length==512);
        CHECK(!std::memcmp(captured.last_attribute_bytes,expected,sizeof(expected)));
        CHECK(captured.successful_writes==2 && captured.last_write_is_toc && captured.memory_status==PS3_GUEST_MEMORY_OK);
        CHECK(captured.publications==event_end-event_begin && control->spurs_attribute_calls==1 && control->spurs_attribute_returns==1);
        const unsigned char toc[]{0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0};
        CHECK(!std::memcmp(after_memory+0x938,toc,8));
        CHECK(recorder.report().published_count == (512+PPU_MEMORY_TRACE_BYTE_CAPACITY-1)/PPU_MEMORY_TRACE_BYTE_CAPACITY+1);
#else
        CHECK(control->stop == StartupStop::UnknownImport);
        CHECK(!std::memcmp(before_memory,after_memory,sizeof(before_memory)));
        CHECK(recorder.report().published_count == 0);
#endif
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) {
    const int result = pthread_mutex_trylock(terminal_guard);
    CHECK(result == 0); if (!result) pthread_mutex_unlock(terminal_guard);
    throw Terminal{};
}
int main() {
    std::printf("SPURS_ATTRIBUTES_LAYOUT cpu=%zu binding=%zu ppu_owner=%zu startup_owner=%zu control=%zu\n", sizeof(ppu_context),sizeof(PpuMemoryBinding),sizeof(PpuMemoryExecution),sizeof(StartupExecution),sizeof(StartupControl));
    { Runtime r; r.execution.ppu.cpu.gpr[10] = 1; r.actual_request(); }
#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
    for (uint64_t exit : {UINT64_C(0x100000000),UINT64_C(0x100),UINT64_C(0xffffffff)}) {
        Runtime r;
        r.call(0x95180230,{UINT64_C(0xabcdd00ff990),UINT64_C(0x123400000000),UINT64_C(0x9999ffffffff),0xffffffff,0xffffffff,0x80000000,exit},{{0xd00ff990,512},{0xd00ff938,8}});
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.word(0xd00ff990)==0 && r.word(0xd00ff994)==0xffffffff && r.word(0xd00ff998)==0xffffffff);
        CHECK(r.word(0xd00ff99c)==0xffffffff && r.word(0xd00ff9a0)==0x80000000);
        CHECK(r.after_memory[0x9a4] == (uint32_t(exit)!=0));
        CHECK(r.control->spurs_attribute.arguments[0]==UINT64_C(0xabcdd00ff990));
    }
    for (uint32_t nid : {0x95180230u,0x07529113u,0xa839a4d9u,0x1051d134u,0x82275c1cu}) {
        Runtime r;
        r.call(nid,{0,0,0},{{0xd00ff938,8}},UINT64_C(0xffffffff80410711));
        CHECK(r.control->stop == StartupStop::None && r.event_end-r.event_begin==1);
        CHECK(r.control->spurs_attribute.attribute_bytes==0 && r.control->spurs_attribute.has_result);
        r.call(nid,{0xd00ff991,0xd0100100,0},{{0xd00ff938,8}},UINT64_C(0xffffffff80410710));
        CHECK(r.control->stop == StartupStop::None);
    }
    { Runtime r; r.call(0x07529113,{0xd00ff991,0,16},{{0xd00ff938,8}},UINT64_C(0xffffffff80410711));
      CHECK(r.control->stop==StartupStop::None); }
    for (uint32_t size : {0u,1u,15u}) {
        Runtime r; r.actual_request();
        const unsigned char prefix[]{'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O'};
        r.seed(0xd0100101,prefix,15);
        r.call(0x07529113,{0xd00ff990,size?0xd0100101u:1u,size},{{0xd00ff9a5,size},{0xd00ff9b4,4},{0xd00ff938,8}});
        CHECK(r.control->stop==StartupStop::None && r.word(0xd00ff9b4)==size);
        CHECK(!std::memcmp(r.after_memory+0x9a5,prefix,size));
        for(size_t i=size;i<15;++i) CHECK(r.after_memory[0x9a5+i]==0);
        CHECK(r.event_end-r.event_begin==(size?3u:2u));
        if(size==15) {
            r.call(0x07529113,{0xd00ff990,0xd0100101,1},{{0xd00ff9a5,1},{0xd00ff9b4,4},{0xd00ff938,8}});
            CHECK(r.word(0xd00ff9b4)==1 && r.after_memory[0x9a6]=='B' && r.after_memory[0x9b3]=='O');
        }
    }
    { Runtime r; r.call(0x07529113,{0xd00ff990,1,16},{{0xd00ff938,8}},UINT64_C(0xffffffff80410702));
      CHECK(r.control->stop==StartupStop::None && r.control->spurs_attribute.attribute_bytes==0); }
    { Runtime r; r.actual_request();
      r.call(0x1051d134,{0xd00ff990},{{0xd00ff9b8,4},{0xd00ff938,8}});
      CHECK(r.word(0xd00ff9b8)==0x10000000 && r.event_end-r.event_begin==2);
      r.call(0xa839a4d9,{0xd00ff990,0x18},{{0xd00ff9b8,4},{0xd00ff938,8}});
      CHECK(r.word(0xd00ff9b8)==0x30000000);
      r.call(0x82275c1c,{0xd00ff990,0xdeadbeef},{{0xd00ff938,8}},UINT64_C(0xffffffff8041070f));
      CHECK(r.control->stop==StartupStop::None && r.word(0xd00ff9bc)==0);
      r.call(0xa839a4d9,{0xd00ff990,0},{{0xd00ff9b8,4},{0xd00ff938,8}});
      CHECK(r.word(0xd00ff9b8)==0x10000000);
      r.call(0x82275c1c,{0xd00ff990,0xdeadbeef},{{0xd00ff9bc,4},{0xd00ff9b8,4},{0xd00ff938,8}});
      CHECK(r.word(0xd00ff9bc)==0xdeadbeef && r.word(0xd00ff9b8)==0x50000000 && r.event_end-r.event_begin==3);
      CHECK(r.events[r.event_begin].address==0xd00ff9bc && r.events[r.event_begin+1].address==0xd00ff9b8);
      r.call(0xa839a4d9,{0xd00ff990,0x18},{{0xd00ff938,8}},UINT64_C(0xffffffff8041070f));
      CHECK(r.control->stop==StartupStop::None && r.word(0xd00ff9b8)==0x50000000);
      r.call(0xa839a4d9,{0xd00ff990,0xffffffff},{{0xd00ff938,8}},UINT64_C(0xffffffff80410702));
      CHECK(r.control->stop==StartupStop::None);
      r.call(0x95180230,{0xd00ff990,2,0x250001,0,0,0,1},{{0xd00ff990,512},{0xd00ff938,8}});
      CHECK(r.word(0xd00ff998)==0 && r.word(0xd00ff9b8)==0 && r.word(0xd00ff9bc)==0 && r.after_memory[0x9a4]==1);
    }
    for (uint32_t container : {0u,0xffffffffu}) {
        Runtime r; r.actual_request(); r.call(0x82275c1c,{0xd00ff990,container},{{0xd00ff9bc,4},{0xd00ff9b8,4},{0xd00ff938,8}});
        CHECK(r.control->stop==StartupStop::None && r.word(0xd00ff9bc)==container && r.word(0xd00ff9b8)==0x40000000);
    }
    for(uint32_t nid:{0x95180230u,0x07529113u,0xa839a4d9u,0x1051d134u,0x82275c1cu}) {
        Runtime r; r.call(nid,{0xfffffff8,nid==0xa839a4d9u?0u:0xd0100100u,15},{});
        CHECK(r.control->stop==StartupStop::InvalidMemory && r.event_end==0);
    }
    { Runtime r; r.call(0xa839a4d9,{0xfffffff8,2},{{0xd00ff938,8}},UINT64_C(0xffffffff80410702)); CHECK(r.control->stop==StartupStop::None); }
    { Runtime r; r.call(0x07529113,{0xfffffff8,0xfffffff8,16},{{0xd00ff938,8}},UINT64_C(0xffffffff80410702)); CHECK(r.control->stop==StartupStop::None); }
    { Runtime r; r.actual_request(); const unsigned char byte=0x7f; r.seed(0xd0100100,&byte,1);
      r.call(0x07529113,{UINT64_C(0x1234d00ff990),UINT64_C(0xabcd0000d0100100),UINT64_C(0x999900000001)},{{0xd00ff9a5,1},{0xd00ff9b4,4},{0xd00ff938,8}});
      CHECK(r.control->stop==StartupStop::None && r.after_memory[0x9a5]==0x7f && r.word(0xd00ff9b4)==1);
      CHECK(r.control->spurs_attribute.arguments[1]==UINT64_C(0xabcd0000d0100100));
      r.call(0xa839a4d9,{0xd00ff990,UINT64_C(0x123400000018)},{{0xd00ff9b8,4},{0xd00ff938,8}});
      CHECK(r.word(0xd00ff9b8)==0x20000000); }
    for (uint32_t address : {0xd00ff938u,0xd0101000u}) {
        Runtime r; r.call(0x95180230,{address,2,0x250001,5,128,249,0},{});
        CHECK(r.control->stop==StartupStop::InvalidMemory && r.event_end==0);
    }
    { Runtime r(64,250,true,12288); r.call(0x95180230,{0xd0101000,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->spurs_attribute.memory_status==PS3_GUEST_MEMORY_UNMAPPED); }
    { Runtime r(64,250,false); r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::InvalidMemory); }
    { Runtime r(64,250,true,8192,true,1,64,true); r.seed_word(0xd00ff9b8,0x40000000);
      // A readable conflicting flag returns STAT before requiring writable flags.
      r.call(0xa839a4d9,{0xd00ff990,0x18},{{0xd00ff938,8}},UINT64_C(0xffffffff8041070f));
      CHECK(r.control->stop==StartupStop::None); }
    for(auto input: {Runtime::Range{0xd00ff9a5,3},Runtime::Range{0xd00ff9a6,3},Runtime::Range{0xd00ff938,1},Runtime::Range{0xfffffff8,15}}) {
        Runtime r; r.call(0x07529113,{0xd00ff990,input.address,input.length},{});
        CHECK(r.control->stop==StartupStop::InvalidMemory && r.event_end==0);
    }
    { Runtime r; r.call(0x07529113,{0xd00ff920,0xd0100100,15},{});
      CHECK(r.control->stop==StartupStop::InvalidMemory); }
    { Runtime r; r.call(0x1051d134,{0xd00ff910},{}); CHECK(r.control->stop==StartupStop::InvalidMemory); }
    { Runtime r; r.call(0x95180230,{0xd00fff00,2,0x250001,5,128,249,0},{{0xd00fff00,512},{0xd00ff938,8}});
      CHECK(r.control->stop==StartupStop::None && r.word(0xd00fff08)==5 && r.word(0xd00fff10)==249); }
    { Runtime r; r.control->cancelled.store(true); r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::Cancelled && r.control->spurs_attribute_calls==0); }
    { Runtime r; r.window.fail_read=true; r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->spurs_attribute.memory_checked && r.control->spurs_attribute.memory_status==PS3_GUEST_MEMORY_UNMAPPED); }
    { Runtime r; r.window.cancel_read=true; r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::Cancelled && !r.control->spurs_attribute.write_attempted); }
    { Runtime r; r.window.cancel_write_address=0xd00ff990; r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{{0xd00ff990,512}});
      CHECK(r.control->stop==StartupStop::Cancelled && r.control->spurs_attribute.attribute_bytes==512 && r.control->spurs_attribute.has_result);
      CHECK(r.control->spurs_attribute_returns==0 && r.control->spurs_attribute.successful_writes==1); }
    { Runtime r; r.window.cancel_write_address=0xd00ff938; r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{{0xd00ff990,512},{0xd00ff938,8}});
      CHECK(r.control->stop==StartupStop::Cancelled && r.control->spurs_attribute.attribute_bytes==512 && r.control->spurs_attribute.last_write_is_toc);
      CHECK(r.control->spurs_attribute_returns==0); }
    { Runtime r; r.window.fail_clock_after_write=true; r.window.clock_trigger_address=0xd00ff990;
      r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{{0xd00ff990,512}});
      CHECK(r.control->stop==StartupStop::BudgetClockFailure && r.control->spurs_attribute.attribute_bytes==512 && r.control->spurs_attribute.has_result); }
    { Runtime r; r.window.partial_write_address=0xd00ff990; r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{{0xd00ff990,2}});
      CHECK(r.control->stop==StartupStop::ProviderFailure && r.control->spurs_attribute.attribute_bytes==0 && !r.control->spurs_attribute.has_result);
      CHECK(r.after_memory[0x990]==0 && r.after_memory[0x991]==0 && r.after_memory[0x992]==0xa5 && r.event_end==0);
      CHECK(r.control->spurs_attribute.memory_status==PS3_GUEST_MEMORY_UNMAPPED); }
    { Runtime r; r.window.fail_write_address=0xd00ff938; r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{{0xd00ff990,512}});
      CHECK(r.control->stop==StartupStop::ProviderFailure && r.control->spurs_attribute.attribute_bytes==512 && r.control->spurs_attribute.last_write_is_toc);
      CHECK(r.control->spurs_attribute.has_result && r.control->spurs_attribute.memory_status==PS3_GUEST_MEMORY_UNMAPPED); }
    for(bool cancelled:{false,true}) {
        Runtime r; r.actual_request(); r.window.cancel_write_address=cancelled?0xd00ff9bc:0;
        r.window.fail_write_address=cancelled?0:0xd00ff9b8;
        r.call(0x82275c1c,{0xd00ff990,0xdeadbeef},{{0xd00ff9bc,4}});
        CHECK(r.control->stop==(cancelled?StartupStop::Cancelled:StartupStop::ProviderFailure));
        CHECK(r.word(0xd00ff9bc)==0xdeadbeef && r.word(0xd00ff9b8)==0 && r.event_end-r.event_begin==1);
        CHECK(!r.control->spurs_attribute.has_result && r.control->spurs_attribute.attribute_bytes==4);
    }
    { Runtime r; r.actual_request(); const unsigned char prefix[3]{1,0,2}; r.seed(0xd0100100,prefix,3);
      r.window.cancel_write_address=0xd00ff9a5;
      r.call(0x07529113,{0xd00ff990,0xd0100100,3},{{0xd00ff9a5,3}});
      CHECK(r.control->stop==StartupStop::Cancelled && r.word(0xd00ff9b4)==0 && r.event_end-r.event_begin==1 && !r.control->spurs_attribute.has_result);
      CHECK(!std::memcmp(r.after_memory+0x9a5,prefix,3)); }
    { Runtime r(0); r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::HleSyscallBudget && r.control->spurs_attribute_calls==0); }
    { const size_t required=(512+PPU_MEMORY_TRACE_BYTE_CAPACITY-1)/PPU_MEMORY_TRACE_BYTE_CAPACITY+1;
      Runtime r(64,250,true,8192,true,1,required-1); r.call(0x95180230,{0xd00ff990,2,0x250001,5,128,249,0},{});
      CHECK(r.control->stop==StartupStop::TraceCapacity && !r.control->spurs_attribute.write_attempted); }
    for(uint32_t reserved:{0xd00ff990u,0xd00ffa00u,0xd00ffa80u,0xd00ffb00u,0xd00ffb80u}) {
        Runtime r; uint32_t value=0; int stored=-1;
        CHECK(ps3_atomic_load_reserve32(&r.domain,r.execution.ppu.reservation,reserved,&value)==PS3_GUEST_MEMORY_OK);
        r.actual_request();
        CHECK(ps3_atomic_store_conditional32(&r.domain,r.execution.ppu.reservation,reserved,123,&stored)==PS3_GUEST_MEMORY_OK && stored==0);
    }
    for(uint32_t reserved:{0xd00ff9b8u,0xd00ffa90u}) {
        Runtime r; r.actual_request(); uint32_t value=0; int stored=-1;
        CHECK(ps3_atomic_load_reserve32(&r.domain,r.execution.ppu.reservation,reserved,&value)==PS3_GUEST_MEMORY_OK);
        r.call(0xa839a4d9,{0xd00ff990,0},{{0xd00ff9b8,4},{0xd00ff938,8}});
        CHECK(r.control->stop==StartupStop::None);
        CHECK(ps3_atomic_store_conditional32(&r.domain,r.execution.ppu.reservation,reserved,123,&stored)==PS3_GUEST_MEMORY_OK);
        CHECK(stored==(reserved==0xd00ffa90u));
    }
    { Runtime a,b; a.actual_request(); b.actual_request(); a.call(0x1051d134,{0xd00ff990},{{0xd00ff9b8,4},{0xd00ff938,8}});
      CHECK(a.word(0xd00ff9b8)==0x10000000 && b.word(0xd00ff9b8)==0); }
    for(uint32_t nid:{0xaa6269a8u,0x30aa96c4u,0x9dcbcb5du}) {
        Runtime r; r.call(nid,{0xd00ff990},{}); CHECK(r.control->stop==StartupStop::UnknownImport && r.control->spurs_attribute_calls==0);
    }
    { unsigned char image[512]; std::memset(image,0xa5,sizeof(image)); uint32_t flags=0x12345678;
      CHECK(uint32_t(ps3_spurs_attribute_validate_prefix(1,0,16))==0x80410711);
      CHECK(uint32_t(ps3_spurs_attribute_validate_group_type(1,2))==0x80410710);
      CHECK(ps3_spurs_attribute_initialize(8,nullptr,image)==PS3_SPURS_ATTRIBUTE_INVALID_HOST_ARGUMENT);
      for(unsigned char byte:image) CHECK(byte==0xa5);
      CHECK(uint32_t(ps3_spurs_attribute_container_flags(0x20000000,&flags))==0x8041070f && flags==0x12345678);
      CHECK(uint32_t(ps3_spurs_attribute_group_flags(0x18,0x40000000,&flags))==0x8041070f && flags==0x12345678);
      CHECK(ps3_spurs_attribute_group_flags(0,0,nullptr)==PS3_SPURS_ATTRIBUTE_INVALID_HOST_ARGUMENT); }
#endif
    std::printf("SPURS_ATTRIBUTES_HOST failures=%d native_runtime=false game_boot=false\n",failures);
    return failures != 0;
}

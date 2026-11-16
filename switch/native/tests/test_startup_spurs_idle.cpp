#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main
#include "startup_spurs_workload_fixture.h"

namespace {
constexpr uint32_t IDLE_NID=0x182d9890;
void call_idle(Runtime &r,uint64_t spurs,uint64_t wid,uint64_t count) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=spurs;cpu.gpr[4]=wid;cpu.gpr[5]=count;r.run(IDLE_NID,false);
}
void pointer_errors() {
    Runtime r;
    call_idle(r,0,99,99);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410711));
    call_idle(r,INSTANCE1+1,99,99);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410710));
}
void real_idle_request_and_errors() {
    IdleServices services;
    {
        Runtime r;services.install(r);r.boot();
        call_idle(r,INSTANCE1,0,7);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410705));
        register_idle_workload(r);
        call_idle(r,INSTANCE1,16,7);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410702));
        call_idle(r,INSTANCE1,0,8);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410702));
        const size_t from=r.published();
        call_idle(r,UINT64_C(0xabcd000000000000)|INSTANCE1,UINT64_C(0x1234000000000000),UINT64_C(0x9876000000000007));
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
        const auto &o=r.control->spurs_idle;
        CHECK(r.control->spurs_idle_calls==4 && r.control->spurs_idle_returns==4);
        CHECK(o.has_domain_result && o.has_result && o.result==0 && o.toc_written);
        CHECK(o.request.spurs==INSTANCE1 && o.request.wid==0 && o.request.count==7);
        CHECK(o.domain_result.reads==3 && o.domain_result.writes==1 && o.domain_result.written_bytes==1);
        CHECK(o.domain_result.idle_byte_write_attempted && o.domain_result.idle_byte_written && o.publications==5);
        CHECK(r.get(INSTANCE1+0x10,1)==7 && r.get(INSTANCE1,1)==0 && r.get(TOC_SLOT,8)==TOC);
        const auto events=r.since(from);
        CHECK(events.size()==5);
        CHECK(events.size()<4 || is_event(events[3],true,INSTANCE1+0x10,1,7));
        CHECK(services.execution_calls.load()==0);
    }
    CHECK(services.execution_calls.load()==0);
}
void trace_refusal_keeps_committed_idle_byte() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r);
    r.leave_trace_capacity(3);
    call_idle(r,INSTANCE1,0,7);
    CHECK(r.control->stop==StartupStop::TraceCapacity);
    const auto &o=r.control->spurs_idle;
    CHECK(o.has_domain_result && !o.has_result && !o.toc_written && o.trace_failure);
    CHECK(o.domain_result.idle_byte_written && o.domain_result.writes==1 && o.domain_result.stop==PS3_SPURS_IDLE_OBSERVER);
    CHECK(r.control->spurs_idle_returns==0);
    CHECK(r.get(INSTANCE1+0x10,1)==7 && r.execution.ppu.cpu.gpr[3]==INSTANCE1);
    CHECK(r.instance().state==PS3_SPURS_INSTANCE_ACTIVE && services.execution_calls.load()==0);
}
}
void no_execution_service_is_native_stop() {
    Runtime r;r.boot();call_idle(r,INSTANCE1,0,7);
    const auto &o=r.control->spurs_idle;
    CHECK(r.control->stop==StartupStop::SpursIdleStop && o.has_domain_result && !o.has_result);
    CHECK(o.domain_result.stop==PS3_SPURS_IDLE_NO_EXECUTION_SERVICES && !o.domain_result.idle_byte_written);
    CHECK(r.get(INSTANCE1+0x10,1)==0 && r.control->spurs_idle_returns==0);
}
void read_only_scope_preserves_sdk_error_order() {
    IdleServices services;Runtime r;services.install(r);r.boot();
    r.ranges[0].writable=false;
    call_idle(r,INSTANCE1,16,7);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410702));
    call_idle(r,INSTANCE1,0,7);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410705));
    r.ranges[0].writable=true;
    register_idle_workload(r);
    r.ranges[0].writable=false;
    const size_t from=r.published();
    call_idle(r,INSTANCE1,0,7);
    CHECK(r.control->stop==StartupStop::InvalidMemory);
    CHECK(r.since(from).size()==3 && r.get(INSTANCE1+0x10,1)==0);
    CHECK(r.control->spurs_idle.has_domain_result && !r.control->spurs_idle.domain_result.idle_byte_written);
    CHECK(r.control->invalid_address==INSTANCE1+0x10 && r.control->spurs_idle.latched_stop==StartupStop::InvalidMemory);
    CHECK(!r.control->spurs_idle.domain_result.idle_byte_write_attempted);
    r.ranges[0].writable=true;
}
void minimal_scopes_admit_only_actual_fields() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r);
    PpuMemoryRamRange permitted[]{{INSTANCE1+0x74,1,false},{INSTANCE1+0xb0,4,false},
        {INSTANCE1+0xd6c,4,false},{INSTANCE1+0x10,1,true},{STACK_BASE,STACK_SPAN,true}};
    PpuMemoryBinding scoped(r.domain,permitted,5);
    PpuMemoryBinding *const original=r.execution.ppu.memory;
    r.execution.ppu.memory=&scoped;
    call_idle(r,INSTANCE1,0,7);
    r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
    CHECK(r.get(INSTANCE1+0x10,1)==7 && r.get(INSTANCE1,1)==0);
    // Omit exception-field admission: two real reads must precede refusal, and no write may occur.
    PpuMemoryRamRange denied[]{{INSTANCE1+0x74,1,false},{INSTANCE1+0xb0,4,false},
        {INSTANCE1+0x10,1,true},{STACK_BASE,STACK_SPAN,true}};
    PpuMemoryBinding missing_read(r.domain,denied,4);
    const size_t from=r.published();
    r.execution.ppu.memory=&missing_read;
    call_idle(r,INSTANCE1,0,3);
    r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==INSTANCE1+0xd6c);
    CHECK(r.since(from).size()==2 && r.control->spurs_idle.domain_result.reads==2);
    CHECK(!r.control->spurs_idle.domain_result.idle_byte_write_attempted && r.get(INSTANCE1+0x10,1)==7);
    CHECK(services.execution_calls.load()==0);
}
int main() {
    pointer_errors();real_idle_request_and_errors();trace_refusal_keeps_committed_idle_byte();no_execution_service_is_native_stop();read_only_scope_preserves_sdk_error_order();minimal_scopes_admit_only_actual_fields();
    std::printf("SPURS_IDLE_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

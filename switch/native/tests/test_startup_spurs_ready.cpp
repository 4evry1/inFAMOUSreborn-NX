#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main
#include "startup_spurs_workload_fixture.h"

namespace {
constexpr uint32_t READY_NID = 0xf843818d;
void call_ready(Runtime &r, uint64_t spurs, uint64_t wid, uint64_t value) {
    auto &cpu=r.execution.ppu.cpu;
    cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=spurs;cpu.gpr[4]=wid;cpu.gpr[5]=value;
    r.run(READY_NID,false);
}
void pointer_errors() {
    Runtime r;
    call_ready(r,0,99,999);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410811));
    call_ready(r,INSTANCE1+1,99,999);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410810));
}
void real_ready_store_and_errors() {
    IdleServices services;Runtime r;services.install(r);r.boot();
    call_ready(r,INSTANCE1,0,7);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410805));
    // Zero priorities admit a real workload that is ineligible for SPU selection.
    register_idle_workload(r,0);
    call_ready(r,INSTANCE1,16,7);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410802));
    call_ready(r,INSTANCE1,0,256);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410802));
    const auto from=r.published();
    call_ready(r,UINT64_C(0xabcd000000000000)|INSTANCE1,UINT64_C(0x1234000000000000),UINT64_C(0x9876000000000007));
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
    CHECK(r.get(INSTANCE1,1)==7 && r.get(INSTANCE1+1,1)==0 && r.get(INSTANCE1+0x10,1)==0);
    CHECK(r.get(TOC_SLOT,8)==TOC);
    const auto &o=r.control->spurs_ready;
    CHECK(r.control->spurs_ready_calls==4 && r.control->spurs_ready_returns==4);
    CHECK(o.has_domain_result && o.has_result && o.toc_written && o.result==0);
    CHECK(o.request.spurs==INSTANCE1 && o.request.wid==0 && o.request.value==7);
    CHECK(o.domain_result.reads==4 && o.domain_result.writes==1 && o.domain_result.written_bytes==1);
    CHECK(o.domain_result.store_attempted && o.domain_result.stored && o.domain_result.release_fences==1 && o.publications==6);
    const auto events=r.since(from);CHECK(events.size()==6);
    CHECK(events.size()<5 || is_event(events[4],true,INSTANCE1,1,7));
    CHECK(services.execution_calls.load()==0);
}
void trace_refusal_keeps_committed_ready_count() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r,0);
    r.leave_trace_capacity(4);call_ready(r,INSTANCE1,0,7);
    CHECK(r.control->stop==StartupStop::TraceCapacity);
    CHECK(r.get(INSTANCE1,1)==7 && r.execution.ppu.cpu.gpr[3]==INSTANCE1);
    const auto &o=r.control->spurs_ready;
    CHECK(o.has_domain_result && !o.has_result && !o.toc_written && o.trace_failure);
    CHECK(o.domain_result.stored && o.domain_result.stop==PS3_SPURS_READY_COUNT_OBSERVER && r.control->spurs_ready_returns==0);
    CHECK(r.instance().state==PS3_SPURS_INSTANCE_ACTIVE && services.execution_calls.load()==0);
}
void read_only_scope_preserves_sdk_error_order() {
    IdleServices services;Runtime r;services.install(r);r.boot();r.ranges[0].writable=false;
    call_ready(r,INSTANCE1,16,7);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410802));
    call_ready(r,INSTANCE1,0,7);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410805));
    r.ranges[0].writable=true;register_idle_workload(r,0);r.ranges[0].writable=false;
    const auto from=r.published();call_ready(r,INSTANCE1,0,7);
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==INSTANCE1);
    CHECK(r.since(from).size()==4 && r.get(INSTANCE1,1)==0);
    r.ranges[0].writable=true;
}
void minimal_scopes_and_missing_read() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r,0);
    PpuMemoryRamRange permitted[]{{INSTANCE1+0x74,1,false},{INSTANCE1+0xb0,4,false},
        {INSTANCE1+0xd6c,4,false},{INSTANCE1+0x80,1,false},{INSTANCE1,1,true},{STACK_BASE,STACK_SPAN,true}};
    PpuMemoryBinding scoped(r.domain,permitted,6);auto *original=r.execution.ppu.memory;r.execution.ppu.memory=&scoped;
    call_ready(r,INSTANCE1,0,255);r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0 && r.get(INSTANCE1,1)==255);
    PpuMemoryRamRange denied[]{{INSTANCE1+0x74,1,false},{INSTANCE1+0xb0,4,false},
        {INSTANCE1+0xd6c,4,false},{INSTANCE1,1,true},{STACK_BASE,STACK_SPAN,true}};
    PpuMemoryBinding missing_state(r.domain,denied,5);const auto from=r.published();r.execution.ppu.memory=&missing_state;
    call_ready(r,INSTANCE1,0,3);r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==INSTANCE1+0x80);
    CHECK(r.since(from).size()==3 && r.get(INSTANCE1,1)==255 && services.execution_calls.load()==0);
}
void missing_services_stops_without_store() {
    Runtime r;r.boot();call_ready(r,INSTANCE1,0,7);
    CHECK(r.control->stop!=StartupStop::None && r.control->stop!=StartupStop::UnknownImport);
    const auto &o=r.control->spurs_ready;
    CHECK(r.control->stop==StartupStop::SpursReadyStop && o.has_domain_result && !o.has_result);
    CHECK(o.domain_result.stop==PS3_SPURS_READY_COUNT_NO_EXECUTION_SERVICES && !o.domain_result.store_attempted);
    CHECK(r.get(INSTANCE1,1)==0 && r.execution.ppu.cpu.gpr[3]==INSTANCE1);
}
}
int main() {
    pointer_errors();real_ready_store_and_errors();trace_refusal_keeps_committed_ready_count();
    read_only_scope_preserves_sdk_error_order();minimal_scopes_and_missing_read();missing_services_stops_without_store();
    std::printf("SPURS_READY_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

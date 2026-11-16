#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main
#include "startup_spurs_workload_fixture.h"
#include <chrono>
#include <thread>

namespace {
constexpr uint32_t SET_EXCEPTION_NID=0xd2e23fa9, UNSET_EXCEPTION_NID=0x4c75deb8;
constexpr uint32_t HANDLER_SLOT=INSTANCE1+0x100, HANDLER=0x7832f8, ARGUMENT=0x915c70;
constexpr uint32_t SDK_NULL=0x80410811, SDK_ALIGN=0x80410810, SDK_INVAL=0x80410802;
constexpr uint32_t SDK_SRCH=0x80410805, SDK_BUSY=0x8041080a;
void call_exception(Runtime &r,uint32_t nid,uint64_t spurs,uint64_t wid,uint64_t handler=0,uint64_t argument=0) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;
    cpu.gpr[3]=spurs;cpu.gpr[4]=wid;cpu.gpr[5]=handler;cpu.gpr[6]=argument;r.run(nid,false);
}
void exception_pointer_precedence() {
    Runtime r;
    call_exception(r,SET_EXCEPTION_NID,0,99,HANDLER);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_NULL));
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1+1,99,0);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_NULL));
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1+1,99,HANDLER);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_ALIGN));
    call_exception(r,UNSET_EXCEPTION_NID,0,0);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_NULL));
}
void exception_registration_and_unset() {
    IdleServices services;Runtime r;services.install(r);r.boot();
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,16,HANDLER);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_INVAL));
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_SRCH));
    register_idle_workload(r);
    uint64_t neighbours[14];for(unsigned i=0;i<14;++i) neighbours[i]=r.get(HANDLER_SLOT+16+i*8,8);
    const size_t from=r.published();
    call_exception(r,SET_EXCEPTION_NID,UINT64_C(0xabcd000000000000)|INSTANCE1,
        UINT64_C(0x1234000000000000),UINT64_C(0xffff000000000000)|HANDLER,UINT64_C(0x9876000000000000)|ARGUMENT);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
    CHECK(r.get(HANDLER_SLOT,8)==HANDLER && r.get(HANDLER_SLOT+8,8)==ARGUMENT && r.get(TOC_SLOT,8)==TOC);
    for(unsigned i=0;i<14;++i) CHECK(neighbours[i]==r.get(HANDLER_SLOT+16+i*8,8));
    const auto events=r.since(from);CHECK(events.size()==8);
    CHECK(events.size()<7 || (is_event(events[4],true,HANDLER_SLOT,8,1) &&
        is_event(events[5],true,HANDLER_SLOT+8,8,ARGUMENT) && is_event(events[6],true,HANDLER_SLOT,8,HANDLER)));
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER+4,ARGUMENT+4);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_BUSY));
    CHECK(r.get(HANDLER_SLOT,8)==HANDLER && r.get(HANDLER_SLOT+8,8)==ARGUMENT);
    call_exception(r,UNSET_EXCEPTION_NID,INSTANCE1,0);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
    CHECK(r.get(HANDLER_SLOT,8)==0 && r.get(HANDLER_SLOT+8,8)==ARGUMENT);
    CHECK(r.get(INSTANCE1,1)==0 && services.execution_calls.load()==0);
}
void exception_observer_refusal_retains_claim() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r);
    r.leave_trace_capacity(4);call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER,ARGUMENT);
    CHECK(r.control->stop==StartupStop::TraceCapacity);
    CHECK(r.get(HANDLER_SLOT,8)==1 && r.get(HANDLER_SLOT+8,8)==0);
    CHECK(r.instance().state==PS3_SPURS_INSTANCE_ACTIVE && r.execution.ppu.cpu.gpr[3]==INSTANCE1);
    CHECK(services.execution_calls.load()==0);
}
void exception_read_only_error_order() {
    IdleServices services;Runtime r;services.install(r);r.boot();
    r.ranges[0].writable=false;
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,16,HANDLER);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_INVAL));
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(SDK_SRCH));
    r.ranges[0].writable=true;register_idle_workload(r);r.ranges[0].writable=false;
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER,ARGUMENT);
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==HANDLER_SLOT);
    CHECK(r.get(HANDLER_SLOT,8)==0 && r.get(HANDLER_SLOT+8,8)==0);
    r.ranges[0].writable=true;
}
void exception_requires_execution_services() {
    Runtime r;r.boot();call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER,ARGUMENT);
    CHECK(r.control->stop==StartupStop::SpursExceptionStop);
    CHECK(r.control->spurs_exception.has_domain_result && !r.control->spurs_exception.has_result);
    CHECK(r.control->spurs_exception.domain_result.stop==PS3_SPURS_EXCEPTION_NO_EXECUTION_SERVICES);
    CHECK(r.get(HANDLER_SLOT,8)==0 && r.get(HANDLER_SLOT+8,8)==0 && r.control->spurs_exception_returns==0);
}
void received_other_port_event_fences_hle() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r);
    uint32_t other_port=0;uint64_t other_source=0;
    CHECK(ps3_lv2_port_create(&r.comp->lv2,&other_port,PS3_EVENT_PORT_LOCAL,0)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_source(&r.comp->lv2,other_port,&other_source)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_connect_local(&r.comp->lv2,other_port,r.instance().queue)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&r.comp->lv2,other_port,0,1,0)==PS3_LV2_OK);
    ps3_spurs_stop_info info{};bool fenced=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!(fenced=ps3_spurs_runtime_poll_stop(&r.comp->runtime,&info)) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::yield();
    CHECK(fenced && info.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT && info.event.source==other_source);
    CHECK(ps3_lv2_port_disconnect(&r.comp->lv2,other_port)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_destroy(&r.comp->lv2,other_port)==PS3_LV2_OK);
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER,ARGUMENT);
    CHECK(r.control->stop==StartupStop::SpursHelperFence && r.control->spurs_fence_latched);
    CHECK(r.control->spurs_fence.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT && r.control->spurs_fence.event.source==other_source);
    CHECK(r.get(HANDLER_SLOT,8)==0 && r.control->spurs_exception_returns==0 && services.execution_calls.load()==0);
}
void exception_minimal_scope() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r);
    PpuMemoryRamRange fields[]{{INSTANCE1+0x70,8,false},{INSTANCE1+0xb0,4,false},{INSTANCE1+0xd6c,4,false},
        {HANDLER_SLOT,16,true},{STACK_BASE,STACK_SPAN,true}};
    PpuMemoryBinding scoped(r.domain,fields,5);auto *original=r.execution.ppu.memory;r.execution.ppu.memory=&scoped;
    call_exception(r,SET_EXCEPTION_NID,INSTANCE1,0,HANDLER,ARGUMENT);r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
    CHECK(r.get(HANDLER_SLOT,8)==HANDLER && r.get(HANDLER_SLOT+8,8)==ARGUMENT && services.execution_calls.load()==0);
}
}
int main() {
    exception_pointer_precedence();exception_registration_and_unset();exception_observer_refusal_retains_claim();
    exception_read_only_error_order();exception_minimal_scope();exception_requires_execution_services();received_other_port_event_fences_hle();
    std::printf("SPURS_EXCEPTION_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

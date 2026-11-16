#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main
#include "startup_spurs_workload_fixture.h"
#include <chrono>
#include <thread>

namespace {
constexpr uint32_t WORKLOAD_INFO_NID = 0x4e153e3e;
constexpr uint32_t INFO_OUT = STACK_BASE + 0x601;
void call_workload_info(Runtime &r, uint64_t spurs, uint64_t wid, uint64_t info) {
    auto &cpu = r.execution.ppu.cpu;
    cpu.gpr[1]=STACK; cpu.gpr[2]=TOC; cpu.gpr[3]=spurs; cpu.gpr[4]=wid; cpu.gpr[5]=info;
    r.run(WORKLOAD_INFO_NID, false);
}
void poison_info(Runtime &r) {
    unsigned char bytes[48]; std::memset(bytes, 0xa5, sizeof bytes); r.put(INFO_OUT,bytes,sizeof bytes);
}
bool wait_for_idle_registration(Runtime &r) {
    auto &kernel=r.instance().kernel;
    constexpr unsigned spu_count=5, active_mask=(1u<<spu_count)-1;
    CHECK(kernel.config.spu_count==spu_count);
    const auto before=r.get(INSTANCE1+0x70,8);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    bool settled=false;
    do {
        const auto control=r.get(INSTANCE1+0x72,2);
        settled=(control & (active_mask<<8))==0 && (control & 0xff)==active_mask &&
            ps3_spurs_k1_phase_of(&kernel)==PS3_SPURS_K1_PHASE_QUIESCENT;
        for(unsigned spu=0;spu<spu_count;++spu)
            settled=settled && ps3_spurs_k1_context_state_of(&kernel,spu)==PS3_SPURS_K1_CTX_BLOCKED_LR;
        if(settled) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    std::printf("WORKLOAD_INFO_QUIESCENCE before=%016llx settled=%016llx reached=%u\n",
        static_cast<unsigned long long>(before),static_cast<unsigned long long>(r.get(INSTANCE1+0x70,8)),unsigned(settled));
    CHECK(settled);
    return settled;
}
void scalar_errors_before_ownership() {
    Runtime r;
    r.execution.spurs=nullptr;
    call_workload_info(r,0,99,INFO_OUT);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410811));
    call_workload_info(r,INSTANCE1+1,99,0);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410811));
    call_workload_info(r,INSTANCE1+1,99,INFO_OUT);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410810));
    CHECK(r.control->spurs_workload_info_calls==3 && r.control->spurs_workload_info_returns==3);
}
void query_without_execution_services() {
    Runtime r; r.boot();
    call_workload_info(r,INSTANCE1,16,INFO_OUT);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410802));
    call_workload_info(r,INSTANCE1,0,INFO_OUT);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x80410805));
    CHECK(r.control->spurs_workload_info.domain_result.reads==2);
    call_workload_info(r,INSTANCE2,0,INFO_OUT);
    CHECK(r.control->stop==StartupStop::SpursWorkloadInfoStop);
    CHECK(r.control->spurs_workload_info.domain_result.stop==PS3_SPURS_WORKLOAD_INFO_UNOWNED_INSTANCE);
    CHECK(!r.control->spurs_workload_info.has_result);
}
void actual_registered_workload_output_and_trace() {
    IdleServices services; Runtime r; services.install(r); r.boot();
    const auto registration_from=r.published();
    register_idle_workload(r,0);
    bool service_message_published=false;
    for(const auto &event:r.since(registration_from))
        service_message_published=service_message_published || is_event(event,true,INSTANCE1+0x72,1,0xff);
    CHECK(service_message_published);
    auto &cpu=r.execution.ppu.cpu;
    cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=INSTANCE1;cpu.gpr[4]=0;cpu.gpr[5]=7;
    r.run(0xf843818d,false); CHECK(r.control->stop==StartupStop::None && cpu.gpr[3]==0);
    // Registration wakes real workers: +72 is cleared per SPU and +73 gains idle bits.
    // Priority zero and no later L0 guest writes make this converged snapshot stable.
    if(!wait_for_idle_registration(r)) return;
    poison_info(r); r.ranges[0].writable=false;
    const auto flags_before=r.get(INSTANCE1+0x70,8), enabled_before=r.get(INSTANCE1+0xb0,4);
    CHECK((enabled_before & 0x80000000)!=0);
    const auto from=r.published();
    call_workload_info(r,UINT64_C(0xabcd000000000000)|INSTANCE1,
        UINT64_C(0x9876000000000000),UINT64_C(0x1234000000000000)|INFO_OUT);
    r.ranges[0].writable=true;
    CHECK(r.control->stop==StartupStop::None && cpu.gpr[3]==0 && cpu.gpr[2]==TOC);
    const auto &o=r.control->spurs_workload_info;
    CHECK(o.has_domain_result && o.has_result && o.toc_written && o.result==0);
    CHECK(o.request.spurs==INSTANCE1 && o.request.wid==0 && o.request.info==INFO_OUT);
    CHECK(o.arguments[2]==(UINT64_C(0x1234000000000000)|INFO_OUT));
    CHECK(o.domain_result.reads==17 && o.domain_result.writes==21 && o.domain_result.written_bytes==46);
    CHECK(o.domain_result.write_attempts==21 && o.domain_result.publications==38 && o.publications==39);
    CHECK(r.get(INFO_OUT,8)==0x404e9300 && r.get(INFO_OUT+8,8)==0);
    CHECK(r.get(INFO_OUT+0x10,4)==0x69c880 && r.get(INFO_OUT+0x14,4)==0x2d80);
    CHECK(r.get(INFO_OUT+0x18,8)==0 && r.get(INFO_OUT+0x28,8)==0);
    CHECK(r.get(INFO_OUT+0x20,1)==0 && r.get(INFO_OUT+0x21,1)==1 && r.get(INFO_OUT+0x22,1)==5);
    CHECK(r.get(INFO_OUT+0x23,1)==7 && r.get(INFO_OUT+0x24,2)==0);
    CHECK(r.get(INFO_OUT+0x26,2)==0xa5a5 && r.get(TOC_SLOT,8)==TOC);
    const auto events=r.since(from); CHECK(events.size()==39);
    if(events.size()==39) {
        CHECK(is_event(events[0],false,INSTANCE1+0x70,8,flags_before));
        CHECK(is_event(events[1],false,INSTANCE1+0xb0,4,enabled_before));
        for(unsigned i=0;i<8;++i) CHECK(is_event(events[3+i],true,INFO_OUT+0x0f-i,1,0));
        CHECK(is_event(events[38],true,TOC_SLOT,8,TOC));
    }
    CHECK(services.execution_calls.load()==0);
}
void trace_failure_retains_first_store() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r,0);poison_info(r);
    r.leave_trace_capacity(3); call_workload_info(r,INSTANCE1,0,INFO_OUT);
    const auto &o=r.control->spurs_workload_info;
    CHECK(r.control->stop==StartupStop::TraceCapacity && o.trace_failure && !o.has_result && !o.toc_written);
    CHECK(o.domain_result.reads==3 && o.domain_result.writes==1 && o.domain_result.written_bytes==1);
    CHECK(o.domain_result.stop==PS3_SPURS_WORKLOAD_INFO_OBSERVER && !o.domain_result.has_sdk_result);
    CHECK(r.get(INFO_OUT+0x0f,1)==0 && r.get(INFO_OUT+0x0e,1)==0xa5 && r.get(INFO_OUT,1)==0xa5);
    CHECK(r.execution.ppu.cpu.gpr[3]==INSTANCE1);
}
void toc_trace_failure_retains_all_output() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r,0);poison_info(r);
    r.leave_trace_capacity(38);call_workload_info(r,INSTANCE1,0,INFO_OUT);
    const auto &o=r.control->spurs_workload_info;
    CHECK(r.control->stop==StartupStop::TraceCapacity && o.domain_result.has_sdk_result && !o.has_result && !o.toc_written);
    CHECK(o.domain_result.writes==21 && r.get(INFO_OUT,8)==0x404e9300 && r.get(INFO_OUT+0x26,2)==0xa5a5);
    CHECK(r.execution.ppu.cpu.gpr[3]==INSTANCE1 && r.control->spurs_workload_info_returns==0);
}
void output_scope_and_toc_overlap_refuse_before_write() {
    IdleServices services;Runtime r;services.install(r);r.boot();register_idle_workload(r,0);poison_info(r);
    PpuMemoryRamRange permitted[]{{INSTANCE1,0x1000,false},{STACK,0x40,true},{INFO_OUT,48,false}};
    PpuMemoryBinding scoped(r.domain,permitted,3);auto *original=r.execution.ppu.memory;r.execution.ppu.memory=&scoped;
    call_workload_info(r,INSTANCE1,0,INFO_OUT);r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==INFO_OUT+0x0f);
    CHECK(r.control->spurs_workload_info.domain_result.reads==3 && r.control->spurs_workload_info.domain_result.writes==0);
    CHECK(r.get(INFO_OUT+0x0f,1)==0xa5);
    call_workload_info(r,INSTANCE1,0,TOC_SLOT-0x0f);
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==TOC_SLOT);
    CHECK(r.control->spurs_workload_info.domain_result.writes==0 && r.get(TOC_SLOT,8)==TOC);
}
}
int main() {
    scalar_errors_before_ownership();query_without_execution_services();actual_registered_workload_output_and_trace();
    trace_failure_retains_first_store();toc_trace_failure_retains_all_output();output_scope_and_toc_overlap_refuse_before_write();
    std::printf("SPURS_WORKLOAD_INFO_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

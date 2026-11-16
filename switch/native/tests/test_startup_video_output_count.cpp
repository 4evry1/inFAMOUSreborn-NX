#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t VIDEO_COUNT_NID=0x75bbb672;
constexpr uint64_t SENTINEL=UINT64_C(0xa5a5a5a5a5a5a5a5);
struct CountMemoryProbe {
    ps3_guest_memory *installed=nullptr,real{};
    StartupControl *control=nullptr;
    unsigned reads=0,writes=0,other=0;
    bool fail_read=false,fail_write=false,cancel_after_read=false,cancel_after_write=false;
    explicit CountMemoryProbe(Runtime &r):installed(&r.guest.backing),real(*installed),control(r.control.get()) {
        *installed={this,commit,decommit,read,write,zero};
    }
    ~CountMemoryProbe() {restore();}
    void restore() {if(installed){*installed=real;installed=nullptr;}}
    static ps3_guest_memory_status commit(void *p,uint32_t a,uint64_t n) {auto &s=*static_cast<CountMemoryProbe*>(p);++s.other;return s.real.commit(s.real.context,a,n);}
    static ps3_guest_memory_status decommit(void *p,uint32_t a,uint64_t n) {auto &s=*static_cast<CountMemoryProbe*>(p);++s.other;return s.real.decommit(s.real.context,a,n);}
    static ps3_guest_memory_status zero(void *p,uint32_t a,uint64_t n) {auto &s=*static_cast<CountMemoryProbe*>(p);++s.other;return s.real.zero(s.real.context,a,n);}
    static ps3_guest_memory_status read(void *p,uint32_t a,void *v,size_t n) {
        auto &s=*static_cast<CountMemoryProbe*>(p);++s.reads;
        if(s.fail_read)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto result=s.real.read(s.real.context,a,v,n);
        if(result==PS3_GUEST_MEMORY_OK && s.cancel_after_read)s.control->cancelled.store(true);
        return result;
    }
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *v,size_t n) {
        auto &s=*static_cast<CountMemoryProbe*>(p);++s.writes;
        if(s.fail_write)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto result=s.real.write(s.real.context,a,v,n);
        if(result==PS3_GUEST_MEMORY_OK && s.cancel_after_write)s.control->cancelled.store(true);
        return result;
    }
};
void seed_toc(Runtime &r) {const unsigned char bytes[8]{0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};r.put(TOC_SLOT,bytes,8);}
void query(Runtime &r,uint64_t selector,uint64_t stack=STACK) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=stack;cpu.gpr[2]=TOC;cpu.gpr[3]=selector;
    for(unsigned i=4;i<=10;++i)cpu.gpr[i]=UINT64_C(0xffffaaaa00000000)+i;
    cpu.lr=0x1158dc;cpu.cr=0x12345678;r.run(VIDEO_COUNT_NID,false);
}
void configured_counts_and_toc_only() {
    Runtime r;r.execution.presentation_topology=PresentationTopology::SingleNativeTarget;
    const uint64_t selectors[]={0,1,UINT64_C(0x1234567800000000),UINT64_C(0x9876543200000001)};
    for(const auto selector:selectors) {
        seed_toc(r);const auto from=r.published();CountMemoryProbe memory(r);query(r,selector);memory.restore();
        const auto &o=r.control->video_output_count;const int32_t expected=uint32_t(selector)==0?1:0;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==uint64_t(int64_t(expected)));
        CHECK(o.raw_video_out==selector && o.video_out==uint32_t(selector));
        CHECK(o.inventory_checked && o.topology==PresentationTopology::SingleNativeTarget);
        CHECK(o.inventory_status==PresentationInventoryStatus::Configured && o.failure==StartupVideoOutputCountFailure::None);
        CHECK(o.count==expected && o.toc_written && o.has_result && o.publications==1 && o.memory_status==PS3_GUEST_MEMORY_OK);
        CHECK(memory.reads==1 && memory.writes==1 && !memory.other);
        CHECK(r.execution.ppu.cpu.gpr[1]==STACK && r.execution.ppu.cpu.gpr[2]==TOC);
        CHECK(r.execution.ppu.cpu.lr==0x1158dc && r.execution.ppu.cpu.cr==0x12345678);
        for(unsigned i=4;i<=10;++i)CHECK(r.execution.ppu.cpu.gpr[i]==UINT64_C(0xffffaaaa00000000)+i);
        const auto events=r.since(from);CHECK(events.size()==1);
        if(events.size()==1)CHECK(is_event(events[0],true,TOC_SLOT,8,TOC));
        CHECK(r.get(TOC_SLOT,8)==TOC);
    }
    CHECK(r.control->video_output_count_calls==4 && r.control->video_output_count_returns==4);
    CHECK(!r.control->system_string_calls && !r.control->disc_change_calls && !r.control->system_parameter_calls);
}
void topology_and_selector_fail_closed() {
    const uint64_t selectors[]={2,UINT32_MAX,UINT64_C(0x100000002)};
    for(unsigned scenario=0;scenario<5;++scenario) {
        Runtime r;seed_toc(r);uint64_t selector=0;
        r.execution.presentation_topology=scenario==0?PresentationTopology::Unconfigured:
            scenario==1?static_cast<PresentationTopology>(99):PresentationTopology::SingleNativeTarget;
        if(scenario>=2)selector=selectors[scenario-2];
        const auto from=r.published();query(r,selector);const auto &o=r.control->video_output_count;
        CHECK(r.control->stop==StartupStop::VideoOutputCountStop && !o.has_result && !o.toc_written && !o.publications);
        CHECK(o.inventory_checked && o.failure==(scenario==0?StartupVideoOutputCountFailure::UnconfiguredTopology:
            scenario==1?StartupVideoOutputCountFailure::InvalidTopology:StartupVideoOutputCountFailure::UnsupportedSelector));
        CHECK(r.execution.ppu.cpu.gpr[3]==selector && r.get(TOC_SLOT,8)==SENTINEL && r.since(from).empty());
        CHECK(r.control->video_output_count_calls==1 && !r.control->video_output_count_returns);
    }
}
void prepublication_failures() {
    for(unsigned scenario=0;scenario<9;++scenario) {
        Runtime r;seed_toc(r);r.execution.presentation_topology=PresentationTopology::SingleNativeTarget;
        CountMemoryProbe memory(r);auto *binding=r.execution.ppu.memory;
        uint64_t stack=STACK;StartupStop expected=StartupStop::InvalidMemory;
        switch(scenario) {
        case 0:r.execution.ppu.memory=nullptr;expected=StartupStop::MissingServices;break;
        case 1:stack=UINT64_MAX-0x20;break;
        case 2:stack=UINT32_MAX;break;
        case 3:r.ranges[1].writable=false;break;
        case 4:memory.fail_read=true;break;
        case 5:memory.cancel_after_read=true;expected=StartupStop::Cancelled;break;
        case 6:r.control->cancelled.store(true);expected=StartupStop::Cancelled;break;
        case 7:r.leave_trace_capacity(0);expected=StartupStop::TraceCapacity;break;
        case 8:tick_calls.store(1000);r.control->deadline_tick=1;expected=StartupStop::Deadline;break;
        }
        query(r,UINT64_C(0x1234000000000000),stack);r.execution.ppu.memory=binding;r.ranges[1].writable=true;memory.restore();
        const auto &o=r.control->video_output_count;
        CHECK(r.control->stop==expected && !o.has_result && !o.toc_written && !o.publications);
        CHECK(!r.control->video_output_count_returns && !memory.writes && r.get(TOC_SLOT,8)==SENTINEL);
        CHECK(r.execution.ppu.cpu.gpr[3]==UINT64_C(0x1234000000000000));
        tick_calls.store(0);
    }
}
void write_failure_and_postwrite_cancellation() {
    for(bool fail:{true,false}) {
        Runtime r;seed_toc(r);r.execution.presentation_topology=PresentationTopology::SingleNativeTarget;
        const auto from=r.published();CountMemoryProbe memory(r);memory.fail_write=fail;memory.cancel_after_write=!fail;
        query(r,UINT64_C(0x1234000000000000));memory.restore();const auto &o=r.control->video_output_count;
        CHECK(r.control->stop==(fail?StartupStop::ProviderFailure:StartupStop::Cancelled));
        CHECK(!o.has_result && !r.control->video_output_count_returns && o.toc_written==!fail);
        CHECK(memory.reads==1 && memory.writes==1 && !memory.other);
        CHECK(o.memory_status==(fail?PS3_GUEST_MEMORY_PLATFORM_FAILURE:PS3_GUEST_MEMORY_OK));
        CHECK(o.publications==(fail?0u:1u) && r.since(from).size()==(fail?0u:1u));
        CHECK(r.get(TOC_SLOT,8)==(fail?SENTINEL:TOC));
        CHECK(r.execution.ppu.cpu.gpr[3]==UINT64_C(0x1234000000000000));
    }
}
void final_trace_credit_and_repeated_topology() {
    Runtime r;r.execution.presentation_topology=PresentationTopology::SingleNativeTarget;r.leave_trace_capacity(1);
    query(r,0);CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==1);
    query(r,1);CHECK(r.control->stop==StartupStop::TraceCapacity && !r.control->video_output_count.has_result);
    CHECK(r.control->video_output_count_calls==2 && r.control->video_output_count_returns==1);
    const auto configured=presentation_inventory(PresentationTopology::SingleNativeTarget);
    CHECK(configured.status==PresentationInventoryStatus::Configured && configured.primary_count==1 && configured.secondary_count==0);
    CHECK(presentation_inventory(PresentationTopology::Unconfigured).status==PresentationInventoryStatus::Unconfigured);
    CHECK(presentation_inventory(static_cast<PresentationTopology>(99)).status==PresentationInventoryStatus::InvalidTopology);
}
}
int main() {
    configured_counts_and_toc_only();topology_and_selector_fail_closed();prepublication_failures();
    write_failure_and_postwrite_cancellation();final_trace_credit_and_repeated_topology();
    std::printf("VIDEO_OUTPUT_COUNT_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

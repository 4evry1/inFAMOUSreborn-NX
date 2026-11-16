#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t BOOT_DISC_NID=0xb72bc4e6, BOOT_OUT=STACK_BASE+0x601;
constexpr unsigned copy_order[32]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,31,28,29,30};
constexpr unsigned char expected_payload[32]={'B','C','U','S','9','8','1','1','9',0,0,0,0,0,0,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
void prepare(Runtime &r) {
    r.execution.boot_content={BootContentOrigin::DiscPackage,"BCUS98119",5};
    unsigned char bytes[34];std::memset(bytes,0xa5,sizeof bytes);r.put(BOOT_OUT-1,bytes,sizeof bytes);
}
void call_disc(Runtime &r,uint64_t output=BOOT_OUT) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=output;
    cpu.gpr[4]=UINT64_C(0x1122334455667788);cpu.gpr[5]=UINT64_C(0x8877665544332211);
    r.run(BOOT_DISC_NID,false);
}
void output_prefix(Runtime &r,unsigned committed) {
    unsigned char expected[32];std::memset(expected,0xa5,sizeof expected);
    if(committed)std::memset(expected,0,4);
    for(unsigned i=1;i<committed && i<=32;++i)expected[copy_order[i-1]]=expected_payload[copy_order[i-1]];
    for(unsigned i=0;i<32;++i)CHECK(r.get(BOOT_OUT+i,1)==expected[i]);
    CHECK(r.get(BOOT_OUT-1,1)==0xa5 && r.get(BOOT_OUT+32,1)==0xa5);
}
void null_and_missing_metadata() {
    Runtime r;prepare(r);r.execution.boot_content={};
    call_disc(r,UINT64_C(0xabcd000000000000));
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x8002bd03));
    CHECK(!r.control->boot_disc.metadata_checked && r.control->boot_disc.output_writes==0);
    output_prefix(r,0);call_disc(r);
    CHECK(r.control->stop==StartupStop::MissingServices && !r.control->boot_disc.has_result);
    CHECK(r.control->boot_disc.failure==StartupBootDiscFailure::MissingMetadata);
    CHECK(r.control->boot_disc.output_writes==1 && !r.control->boot_disc.toc_written);
    output_prefix(r,1);
}
void exact_output_and_order() {
    Runtime r;prepare(r);const auto from=r.published();call_disc(r,UINT64_C(0x5678000000000000)|BOOT_OUT);
    const auto &o=r.control->boot_disc;
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
    CHECK(r.execution.ppu.cpu.gpr[4]==UINT64_C(0x1122334455667788) && r.execution.ppu.cpu.gpr[5]==UINT64_C(0x8877665544332211));
    CHECK(o.output==BOOT_OUT && o.raw_output==(UINT64_C(0x5678000000000000)|BOOT_OUT));
    CHECK(o.metadata_checked && o.has_result && o.toc_written && o.result==0);
    CHECK(o.output_writes==33 && o.output_write_attempts==33 && o.output_written_bytes==36 && o.publications==34);
    CHECK(r.control->boot_disc_calls==1 && r.control->boot_disc_returns==1);output_prefix(r,33);
    const auto events=r.since(from);CHECK(events.size()==34);
    if(events.size()==34) {
        CHECK(is_event(events[0],true,BOOT_OUT,4,0));
        for(unsigned i=0;i<32;++i)CHECK(is_event(events[i+1],true,BOOT_OUT+copy_order[i],1,expected_payload[copy_order[i]]));
        CHECK(is_event(events[33],true,TOC_SLOT,8,TOC));
    }
}
void nondisc_and_invalid_metadata() {
    Runtime r;prepare(r);r.execution.boot_content.origin=BootContentOrigin::InstalledPackage;call_disc(r);
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(0x8002bd02));
    CHECK(r.control->boot_disc.output_writes==1 && r.control->boot_disc.has_result);output_prefix(r,1);
    prepare(r);r.execution.boot_content.title_id[9]='X';call_disc(r);
    CHECK(r.control->stop==StartupStop::BootDiscStop && !r.control->boot_disc.has_result);
    CHECK(r.control->boot_disc.failure==StartupBootDiscFailure::InvalidMetadata);output_prefix(r,1);
    prepare(r);r.execution.boot_content.origin=static_cast<BootContentOrigin>(99);call_disc(r);
    CHECK(r.control->stop==StartupStop::BootDiscStop && !r.control->boot_disc.has_result);output_prefix(r,1);
}
struct Fault {
    ps3_guest_memory real{};StartupControl *control=nullptr;unsigned attempts=0,fail_at=0,cancel_after=0;
    static ps3_guest_memory_status read(void *p,uint32_t a,void *v,size_t n) {auto &s=*static_cast<Fault*>(p);return s.real.read(s.real.context,a,v,n);}
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *v,size_t n) {
        auto &s=*static_cast<Fault*>(p);if(++s.attempts==s.fail_at)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto result=s.real.write(s.real.context,a,v,n);
        if(result==PS3_GUEST_MEMORY_OK && s.attempts==s.cancel_after)s.control->cancelled.store(true);
        return result;
    }
};
void provider_failure_and_cancellation_prefixes() {
    for(bool cancel:{false,true})for(unsigned ordinal=1;ordinal<=34;++ordinal) {
        Runtime r;prepare(r);auto &memory=r.guest.backing;
        Fault fault{memory,r.control.get(),0,cancel?0:ordinal,cancel?ordinal:0};
        memory.context=&fault;memory.read=Fault::read;memory.write=Fault::write;
        call_disc(r);memory=fault.real;
        const unsigned committed=cancel?ordinal:ordinal-1;
        CHECK(r.control->stop==(cancel?StartupStop::Cancelled:StartupStop::ProviderFailure));
        CHECK(!r.control->boot_disc.has_result && r.control->boot_disc_returns==0);
        CHECK(r.control->boot_disc.output_writes==(committed<33?committed:33));
        CHECK(r.control->boot_disc.toc_written==(committed==34));
        output_prefix(r,committed<33?committed:33);
    }
}
void trace_capacity_prefixes() {
    for(unsigned capacity=0;capacity<34;++capacity) {
        Runtime r;prepare(r);r.leave_trace_capacity(capacity);call_disc(r);
        CHECK(r.control->stop==StartupStop::TraceCapacity && !r.control->boot_disc.has_result);
        CHECK(r.control->boot_disc.output_writes==capacity && !r.control->boot_disc.toc_written);
        output_prefix(r,capacity);
    }
}
void scope_and_toc_overlap() {
    Runtime r;prepare(r);
    PpuMemoryRamRange ranges[]{{BOOT_OUT,10,true},{STACK,0x40,true}};
    PpuMemoryBinding scoped(r.domain,ranges,2);auto *original=r.execution.ppu.memory;r.execution.ppu.memory=&scoped;
    call_disc(r);r.execution.ppu.memory=original;
    CHECK(r.control->stop==StartupStop::InvalidMemory && r.control->invalid_address==BOOT_OUT+10);
    CHECK(r.control->boot_disc.output_writes==11);output_prefix(r,11);
    call_disc(r,TOC_SLOT-3);
    CHECK(r.control->stop==StartupStop::InvalidMemory && !r.control->boot_disc.output_writes);
    CHECK(!r.control->boot_disc.has_result);
}
}
int main() {
    null_and_missing_metadata();exact_output_and_order();nondisc_and_invalid_metadata();
    provider_failure_and_cancellation_prefixes();trace_capacity_prefixes();scope_and_toc_overlap();
    std::printf("BOOT_DISC_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

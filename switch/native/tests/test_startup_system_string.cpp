#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t STRING_NID=0x938013a0, STRING_OUT=STACK_BASE+0x601;
constexpr uint32_t STRING_VALUE_ERROR=0x8002b102, STRING_SIZE_ERROR=0x8002b103;
void prepare_strings(Runtime &r) {
    r.execution.system_profile={SystemProfileSource::NativeApplicationProfile,"Player","Nintendo Switch"};
    unsigned char bytes[130];std::memset(bytes,0xa5,sizeof bytes);r.put(STRING_OUT-1,bytes,sizeof bytes);
}
void query_string(Runtime &r,uint64_t id=0x131,uint64_t size=64,uint64_t output=STRING_OUT) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;
    cpu.gpr[3]=id;cpu.gpr[4]=output;cpu.gpr[5]=size;cpu.gpr[6]=UINT64_C(0x123456789abcdef0);
    r.run(STRING_NID,false);
}
void check_string_bytes(Runtime &r,const char *expected,size_t length,size_t written) {
    unsigned char bytes[130]{};auto memory=r.mem();
    CHECK(memory.read(memory.context,STRING_OUT-1,bytes,sizeof bytes)==PS3_GUEST_MEMORY_OK);
    CHECK(bytes[0]==0xa5);
    for(size_t i=0;i<129;++i)
        CHECK(bytes[i+1]==(i<written?(i<length?static_cast<unsigned char>(expected[i]):0):0xa5));
}
void actual_queries_and_padding() {
    Runtime r;prepare_strings(r);
    for(unsigned which=0;which<2;++which) {
        const uint32_t id=which?0x113:0x131,n=which?128:64;
        const char *text=which?"Nintendo Switch":"Player";
        prepare_strings(r);const auto from=r.published();
        query_string(r,UINT64_C(0xabcd000000000000)|id,UINT64_C(0x7654000000000000)|n,UINT64_C(0x9876000000000000)|STRING_OUT);
        const auto &o=r.control->system_string;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
        CHECK(r.execution.ppu.cpu.gpr[6]==UINT64_C(0x123456789abcdef0));
        CHECK(o.id==id && o.size==n && o.output==STRING_OUT);
        CHECK(o.raw_id==(UINT64_C(0xabcd000000000000)|id) && o.raw_size==(UINT64_C(0x7654000000000000)|n));
        CHECK(o.raw_output==(UINT64_C(0x9876000000000000)|STRING_OUT));
        CHECK(o.profile_checked && o.text_checked && o.profile_status==SystemProfileStatus::Valid && o.source==SystemProfileSource::NativeApplicationProfile);
        CHECK(o.text_length==std::strlen(text) && o.output_write_attempts==n && o.output_written_bytes==n);
        CHECK(o.publications==n+1 && o.toc_written && o.has_result && o.result==0);
        check_string_bytes(r,text,std::strlen(text),n);const auto events=r.since(from);CHECK(events.size()==n+1);
        if(events.size()==n+1) {
            for(unsigned i=0;i<n;++i)CHECK(is_event(events[i],true,STRING_OUT+i,1,i<std::strlen(text)?static_cast<unsigned char>(text[i]):0));
            CHECK(is_event(events[n],true,TOC_SLOT,8,TOC));
        }
    }
    CHECK(r.control->system_string_calls==2 && r.control->system_string_returns==2);
    CHECK(r.control->system_parameter_calls==0 && r.control->boot_disc_calls==0);
}
void validation_and_wrapper_errors() {
    for(uint32_t id:{0x131u,0x113u})for(uint32_t size:{0u,63u,65u,127u,129u,UINT32_MAX}) {
        Runtime r;prepare_strings(r);query_string(r,id,size);
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(STRING_SIZE_ERROR));
        CHECK(r.control->system_string.has_result && r.control->system_string.output_write_attempts==0);
        check_string_bytes(r,"",0,0);
    }
    for(bool unterminated:{false,true}) {
        Runtime r;prepare_strings(r);auto &text=r.execution.system_profile.current_user_display_name;
        if(unterminated)std::memset(text,'x',sizeof text);
        else {text[0]=char(0xc0);text[1]=char(0x80);}
        query_string(r,0x131,63);
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(STRING_SIZE_ERROR));
        CHECK(r.control->system_string.profile_checked && !r.control->system_string.text_checked);
        CHECK(r.control->system_string.output_write_attempts==0);check_string_bytes(r,"",0,0);
    }
    for(uint32_t id:{0x131u,0x999u}) {
        Runtime r;prepare_strings(r);r.execution.system_profile={};query_string(r,id,64,UINT64_C(0xffff000000000000));
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(STRING_VALUE_ERROR));
        CHECK(!r.control->system_string.profile_checked);check_string_bytes(r,"",0,0);
    }
    for(uint32_t id:{0u,0x999u,UINT32_MAX}) {
        Runtime r;prepare_strings(r);query_string(r,id,64,UINT32_MAX);
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(STRING_VALUE_ERROR));
        CHECK(r.control->system_string.output_write_attempts==0);check_string_bytes(r,"",0,0);
    }
    for(uint32_t id:{0x1008u,0x1011u,0x1012u,0x1024u}) {
        Runtime r;prepare_strings(r);query_string(r,id);
        CHECK(r.control->stop==StartupStop::SystemStringStop && !r.control->system_string.has_result);
        CHECK(r.control->system_string.failure==StartupSystemStringFailure::UnimplementedParameter);
        check_string_bytes(r,"",0,0);
    }
    for(unsigned scenario=0;scenario<5;++scenario) {
        Runtime r;prepare_strings(r);auto &p=r.execution.system_profile;
        if(scenario==0)p={};
        if(scenario==1)p.source=static_cast<SystemProfileSource>(99);
        if(scenario==2)std::memset(p.current_user_display_name,'x',64);
        if(scenario==3){p.current_user_display_name[0]=char(0xc0);p.current_user_display_name[1]=char(0x80);}
        if(scenario==4){p.source=SystemProfileSource::Unconfigured;}
        query_string(r,scenario==4?0x999:0x131);
        CHECK(r.control->stop==StartupStop::SystemStringStop && !r.control->system_string.has_result);
        CHECK(r.control->system_string.failure==StartupSystemStringFailure::InvalidProfile);
        CHECK(r.control->system_string.output_write_attempts==0);check_string_bytes(r,"",0,0);
    }
    for(unsigned scenario=0;scenario<3;++scenario) {
        Runtime r;prepare_strings(r);auto &p=r.execution.system_profile;
        std::memset(p.device_display_label,0xff,sizeof p.device_display_label);
        std::memset(p.current_user_display_name,0xff,sizeof p.current_user_display_name);
        if(scenario==0)p.current_user_display_name[0]=0;
        if(scenario==1)std::memcpy(p.current_user_display_name,"\xc3\xa9\xe6\x97\xa5\xf0\x9f\x8e\xae",10);
        if(scenario==2){std::memset(p.current_user_display_name,'x',63);p.current_user_display_name[63]=0;}
        query_string(r);CHECK(r.control->stop==StartupStop::None && r.control->system_string.has_result);
        check_string_bytes(r,p.current_user_display_name,std::strlen(p.current_user_display_name),64);
    }
}
struct StringFault {
    ps3_guest_memory real{};StartupControl *control=nullptr;unsigned attempts=0,fail_at=0,cancel_after=0;
    static ps3_guest_memory_status read(void *p,uint32_t a,void *v,size_t n) {auto &s=*static_cast<StringFault*>(p);return s.real.read(s.real.context,a,v,n);}
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *v,size_t n) {
        auto &s=*static_cast<StringFault*>(p);if(++s.attempts==s.fail_at)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto result=s.real.write(s.real.context,a,v,n);
        if(result==PS3_GUEST_MEMORY_OK && s.attempts==s.cancel_after)s.control->cancelled.store(true);
        return result;
    }
};
struct FinalPageGuestSpace {
    GuestSpace base;
    static bool map(uint32_t address,uint64_t length,uint32_t &host) {
        constexpr uint32_t final_page=0xfffff000u;
        if(address>=final_page && length<=UINT64_C(0x100000000)-address) {
            host=HOST_STACK+STACK_SPAN+(address-final_page);return true;
        }
        return GuestSpace::map(address,length,host);
    }
#define FINAL_PAGE_SPAN(name) static ps3_guest_memory_status name(void *p,uint32_t a,uint64_t n) { \
        auto &memory=static_cast<FinalPageGuestSpace*>(p)->base.backing;uint32_t host; \
        return map(a,n,host)?memory.name(memory.context,host,n):PS3_GUEST_MEMORY_INVALID_RANGE; }
    FINAL_PAGE_SPAN(commit) FINAL_PAGE_SPAN(decommit) FINAL_PAGE_SPAN(zero)
#undef FINAL_PAGE_SPAN
    static ps3_guest_memory_status read(void *p,uint32_t a,void *value,size_t n) {
        auto &memory=static_cast<FinalPageGuestSpace*>(p)->base.backing;uint32_t host;
        return map(a,n,host)?memory.read(memory.context,host,value,n):PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *value,size_t n) {
        auto &memory=static_cast<FinalPageGuestSpace*>(p)->base.backing;uint32_t host;
        return map(a,n,host)?memory.write(memory.context,host,value,n):PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    ps3_guest_memory port() {return {this,commit,decommit,read,write,zero};}
};
void admitted_address_wrap_retains_prefix() {
    FinalPageGuestSpace guest;ps3_pthread_guard guard{};CHECK(ps3_pthread_guard_init(&guard)==0);
    ps3_atomic_domain domain{};ps3_atomic_owner_record owners[2]{};ps3_atomic_pin_record pins[8]{};
    CHECK(ps3_atomic_domain_init(&domain,guest.port(),ps3_pthread_guard_port(&guard),owners,2,pins,8)==PS3_GUEST_MEMORY_OK);
    PpuMemoryRamRange ranges[]={{0,0x1000,true},{STACK_BASE,STACK_SPAN,true},{0xfffff000u,0x1000,true}};
    PpuMemoryBinding binding(domain,ranges,3);
    for(const auto &range:ranges)CHECK(ps3_atomic_commit(&domain,range.address,range.length)==PS3_GUEST_MEMORY_OK);
    Ps3PpuExecutionServices services{};services.memory=&binding.coherent_memory;
    std::unique_ptr<StartupControl> control{new StartupControl};StartupExecution execution{};execution.control=control.get();
    PpuMemoryTraceEvent events[128]{};PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(events,128,PpuMemoryTraceRetention::Strict,0));
    ps3_clock clock{nullptr,Runtime::sample};infamous::discovery::DiscoveryBudget policy{{64,64,64,UINT64_C(1000000000)},0};
    DiscoveryBudgetBinding budget(policy,clock);CHECK(native_startup_admit_budgeted(execution,binding,services,recorder,budget));
    const auto &memory=binding.coherent_memory;unsigned char before[64];std::memset(before,0xa5,sizeof before);
    CHECK(memory.write(memory.context,0,before,sizeof before)==PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context,UINT32_MAX-1,before,2)==PS3_GUEST_MEMORY_OK);
    execution.system_profile={SystemProfileSource::NativeApplicationProfile,"Player","Nintendo Switch"};
    auto &cpu=execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=0x131;cpu.gpr[4]=UINT32_MAX-1;cpu.gpr[5]=64;
    active=control.get();try {ps3_hle_call(STRING_NID,&cpu);active=nullptr;}catch(const Terminal &){}
    CHECK(control->stop==StartupStop::InvalidMemory && control->invalid_address==UINT64_C(0x100000000));
    CHECK(control->system_string.output_written_bytes==2 && control->system_string.output_write_attempts==2);
    CHECK(!control->system_string.toc_written && !control->system_string.has_result && control->system_string_returns==0);
    unsigned char tail[2]{},low[64]{};
    CHECK(memory.read(memory.context,UINT32_MAX-1,tail,2)==PS3_GUEST_MEMORY_OK && tail[0]=='P' && tail[1]=='l');
    CHECK(memory.read(memory.context,0,low,sizeof low)==PS3_GUEST_MEMORY_OK && std::memcmp(low,before,sizeof low)==0);
    CHECK(native_startup_release_after_join(execution)==PS3_GUEST_MEMORY_OK);
    ps3_pthread_guard_destroy(&guard);
}
void retained_effects_and_scope() {
    for(bool cancel:{false,true})for(unsigned ordinal=1;ordinal<=129;++ordinal) {
        Runtime r;prepare_strings(r);auto &memory=r.guest.backing;
        StringFault fault{memory,r.control.get(),0,cancel?0:ordinal,cancel?ordinal:0};
        memory.context=&fault;memory.read=StringFault::read;memory.write=StringFault::write;
        query_string(r,0x113,128);memory=fault.real;const unsigned committed=cancel?ordinal:ordinal-1;
        CHECK(r.control->stop==(cancel?StartupStop::Cancelled:StartupStop::ProviderFailure));
        CHECK(r.control->system_string.output_written_bytes==std::min(committed,128u));
        CHECK(r.control->system_string.toc_written==(committed==129));
        CHECK(!r.control->system_string.has_result && r.control->system_string_returns==0);
        check_string_bytes(r,"Nintendo Switch",15,std::min(committed,128u));
    }
    for(unsigned credit=0;credit<=128;++credit) {
        Runtime r;prepare_strings(r);r.leave_trace_capacity(credit);query_string(r,0x113,128);
        CHECK(r.control->stop==StartupStop::TraceCapacity && !r.control->system_string.has_result);
        CHECK(r.control->system_string.output_written_bytes==credit);
        check_string_bytes(r,"Nintendo Switch",15,credit);
    }
    for(uint32_t output:{TOC_SLOT,UINT32_MAX,0x90000000u}) {
        Runtime r;prepare_strings(r);query_string(r,0x131,64,output);
        CHECK(r.control->stop==StartupStop::InvalidMemory && !r.control->system_string.has_result);
        CHECK(r.control->system_string.output_write_attempts==0);check_string_bytes(r,"",0,0);
    }
    Runtime r;prepare_strings(r);r.put8(TOC_SLOT-1,0xa5);query_string(r,0x131,64,TOC_SLOT-1);
    CHECK(r.control->stop==StartupStop::InvalidMemory && !r.control->system_string.has_result);
    CHECK(r.control->system_string.output_written_bytes==1 && r.get(TOC_SLOT-1,1)=='P');
}
}
int main() {
    actual_queries_and_padding();validation_and_wrapper_errors();retained_effects_and_scope();admitted_address_wrap_retains_prefix();
    std::printf("SYSTEM_STRING_HLE_FAILURES=%d\n",failures);return failures?1:0;
}

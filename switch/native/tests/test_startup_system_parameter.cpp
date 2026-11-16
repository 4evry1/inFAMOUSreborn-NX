#define main previous_spurs_attach_tests_main
#include "test_startup_spurs_attach.cpp"
#undef main

namespace {
constexpr uint32_t PARAM_NID=0x40e895d3, PARAM_OUT=STACK_BASE+0x601;
constexpr uint32_t PARAM_VALUE_ERROR=0x8002b102;
void prepare_parameters(Runtime &r) {
    r.execution.system_parameters={
        {SystemLanguageStatus::Available,SystemLanguage::EnglishUS},
        SystemLanguageCompatibility::NativePortLegacyFamilies,
        SystemParameterProfile::NativePortConventions,
        SystemConfirmButton::Cross,SystemDateFormat::YearMonthDay,SystemTimeFormat::TwentyFourHour,0};
    unsigned char bytes[6];std::memset(bytes,0xa5,sizeof bytes);r.put(PARAM_OUT-1,bytes,sizeof bytes);
}
void query_parameter(Runtime &r,uint64_t id,uint64_t output=PARAM_OUT) {
    auto &cpu=r.execution.ppu.cpu;cpu.gpr[1]=STACK;cpu.gpr[2]=TOC;cpu.gpr[3]=id;cpu.gpr[4]=output;
    cpu.gpr[5]=UINT64_C(0x123456789abcdef0);r.run(PARAM_NID,false);
}
void check_parameter_output(Runtime &r,uint32_t expected) {
    CHECK(r.get(PARAM_OUT,4)==expected);
    CHECK(r.get(PARAM_OUT-1,1)==0xa5 && r.get(PARAM_OUT+4,1)==0xa5);
}
void five_real_query_shapes() {
    Runtime r;prepare_parameters(r);const uint32_t ids[]={0x111,0x112,0x114,0x115,0x121};
    const uint32_t values[]={1,1,0,1,0};
    for(unsigned i=0;i<5;++i) {
        const auto from=r.published();query_parameter(r,UINT64_C(0xabcd000000000000)|ids[i],UINT64_C(0x9876000000000000)|PARAM_OUT);
        const auto &o=r.control->system_parameter;
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==0);
        CHECK(r.execution.ppu.cpu.gpr[5]==UINT64_C(0x123456789abcdef0));
        CHECK(o.id==ids[i] && o.output==PARAM_OUT && o.raw_id==(UINT64_C(0xabcd000000000000)|ids[i]));
        CHECK(o.raw_output==(UINT64_C(0x9876000000000000)|PARAM_OUT));
        CHECK(o.output_write_attempts==1 && o.output_written && o.toc_written && o.publications==2 && o.has_result && o.result==0);
        check_parameter_output(r,values[i]);const auto events=r.since(from);CHECK(events.size()==2);
        if(events.size()==2) {CHECK(is_event(events[0],true,PARAM_OUT,4,values[i]));CHECK(is_event(events[1],true,TOC_SLOT,8,TOC));}
    }
    CHECK(r.control->system_parameter_calls==5 && r.control->system_parameter_returns==5);
}
void null_invalid_and_unimplemented() {
    Runtime r;prepare_parameters(r);r.execution.system_parameters={};
    query_parameter(r,0x111,UINT64_C(0xffff000000000000));
    CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(PARAM_VALUE_ERROR));
    CHECK(!r.control->system_parameter.settings_checked && !r.control->system_parameter.output_written);
    check_parameter_output(r,0xa5a5a5a5);
    for(uint32_t id:{0x124u,0x122u,0x999u,0xffffffffu}) {
        query_parameter(r,id,UINT32_MAX);
        CHECK(r.control->stop==StartupStop::None && r.execution.ppu.cpu.gpr[3]==rc_reg(PARAM_VALUE_ERROR));
        CHECK(!r.control->system_parameter.output_written);check_parameter_output(r,0xa5a5a5a5);
    }
    for(uint32_t id:{0x116u,0x123u,0x141u,0x2101u}) {
        query_parameter(r,id);
        CHECK(r.control->stop==StartupStop::SystemParameterStop && !r.control->system_parameter.has_result);
        CHECK(r.control->system_parameter.failure==StartupSystemParameterFailure::UnimplementedParameter);
        check_parameter_output(r,0xa5a5a5a5);
    }
    r.execution.process_sdk={0x23ffff,true};query_parameter(r,0x122);
    CHECK(r.control->stop==StartupStop::SystemParameterStop && !r.control->system_parameter.has_result);
}
void sdk_selectors_and_explicit_projection() {
    struct Case {uint32_t raw;bool valid;uint32_t selector;};
    for(const Case c: {Case{0,true,0x2101},Case{0x30ffff,true,0x2101},Case{0x310000,true,0x2102},
            Case{0x3fffff,true,0x2102},Case{0x400000,true,0x2103},Case{0x42ffff,true,0x2103},
            Case{0x430000,true,0x111},Case{UINT32_MAX,true,0x111},Case{0,false,0x111}}) {
        Runtime r;prepare_parameters(r);r.execution.process_sdk={c.raw,c.valid};query_parameter(r,0x111);
        CHECK(r.control->stop==StartupStop::None && r.control->system_parameter.effective_id==c.selector);
        CHECK(r.control->system_parameter.process_sdk.raw==c.raw && r.control->system_parameter.process_sdk.valid==c.valid);
        check_parameter_output(r,1);
    }
    for(uint32_t value=0;value<20;++value) {
        Runtime r;prepare_parameters(r);r.execution.system_parameters.language.language=static_cast<SystemLanguage>(value);
        query_parameter(r,0x111);
        if(value==19) {
            CHECK(r.control->stop==StartupStop::SystemParameterStop && !r.control->system_parameter.has_result);
            check_parameter_output(r,0xa5a5a5a5);
        } else {
            CHECK(r.control->stop==StartupStop::None);
            check_parameter_output(r,value==17?7:value==18?1:value);
            CHECK(r.control->system_parameter.legacy_projection==(value>=17));
        }
    }
    Runtime r;prepare_parameters(r);r.execution.process_sdk={0x430000,true};
    r.execution.system_parameters.language.language=SystemLanguage::Turkish;query_parameter(r,0x111);
    CHECK(r.control->stop==StartupStop::None);check_parameter_output(r,19);
}
void unavailable_and_invalid_native_values() {
    for(auto status:{SystemLanguageStatus::Unavailable,SystemLanguageStatus::Unsupported,static_cast<SystemLanguageStatus>(99)}) {
        Runtime r;prepare_parameters(r);r.execution.system_parameters.language.status=status;query_parameter(r,0x111);
        CHECK(r.control->stop==StartupStop::SystemParameterStop && !r.control->system_parameter.output_written && !r.control->system_parameter.has_result);
        check_parameter_output(r,0xa5a5a5a5);
    }
    for(unsigned scenario=0;scenario<8;++scenario) {
        Runtime r;prepare_parameters(r);auto &s=r.execution.system_parameters;uint32_t id=0x111;
        switch(scenario) {
        case 0:s.language.language=static_cast<SystemLanguage>(99);break;
        case 1:s.language_compatibility=SystemLanguageCompatibility::Unconfigured;break;
        case 2:s.language_compatibility=static_cast<SystemLanguageCompatibility>(99);break;
        case 3:s.profile=SystemParameterProfile::Unavailable;id=0x112;break;
        case 4:s.confirm_button=static_cast<SystemConfirmButton>(2);id=0x112;break;
        case 5:s.date_format=static_cast<SystemDateFormat>(3);id=0x114;break;
        case 6:s.time_format=static_cast<SystemTimeFormat>(2);id=0x115;break;
        case 7:s.parental_level=12;id=0x121;break;
        }
        query_parameter(r,id);CHECK(r.control->stop==StartupStop::SystemParameterStop && !r.control->system_parameter.has_result);
        CHECK(!r.control->system_parameter.output_written);check_parameter_output(r,0xa5a5a5a5);
    }
    Runtime r;prepare_parameters(r);r.execution.system_parameters.profile=static_cast<SystemParameterProfile>(99);query_parameter(r,0x112);
    CHECK(r.control->stop==StartupStop::SystemParameterStop);check_parameter_output(r,0xa5a5a5a5);
}
struct ParameterFault {
    ps3_guest_memory real{};StartupControl *control=nullptr;unsigned attempts=0,fail_at=0,cancel_after=0;
    static ps3_guest_memory_status read(void *p,uint32_t a,void *v,size_t n) {auto &s=*static_cast<ParameterFault*>(p);return s.real.read(s.real.context,a,v,n);}
    static ps3_guest_memory_status write(void *p,uint32_t a,const void *v,size_t n) {
        auto &s=*static_cast<ParameterFault*>(p);if(++s.attempts==s.fail_at)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto result=s.real.write(s.real.context,a,v,n);
        if(result==PS3_GUEST_MEMORY_OK && s.attempts==s.cancel_after)s.control->cancelled.store(true);
        return result;
    }
};
void retained_effects_and_scope() {
    for(bool cancel:{false,true})for(unsigned ordinal=1;ordinal<=2;++ordinal) {
        Runtime r;prepare_parameters(r);auto &memory=r.guest.backing;
        ParameterFault fault{memory,r.control.get(),0,cancel?0:ordinal,cancel?ordinal:0};
        memory.context=&fault;memory.read=ParameterFault::read;memory.write=ParameterFault::write;
        query_parameter(r,0x111);memory=fault.real;const unsigned committed=cancel?ordinal:ordinal-1;
        CHECK(r.control->stop==(cancel?StartupStop::Cancelled:StartupStop::ProviderFailure));
        CHECK(r.control->system_parameter.output_written==(committed>=1) && r.control->system_parameter.toc_written==(committed==2));
        CHECK(!r.control->system_parameter.has_result && r.control->system_parameter_returns==0);
        check_parameter_output(r,committed?1:0xa5a5a5a5);
    }
    for(unsigned credit=0;credit<2;++credit) {
        Runtime r;prepare_parameters(r);r.leave_trace_capacity(credit);query_parameter(r,0x111);
        CHECK(r.control->stop==StartupStop::TraceCapacity && !r.control->system_parameter.has_result);
        CHECK(r.control->system_parameter.output_written==(credit==1));check_parameter_output(r,credit?1:0xa5a5a5a5);
    }
    for(uint32_t output:{TOC_SLOT-3,UINT32_MAX-2,0x90000000u}) {
        Runtime r;prepare_parameters(r);query_parameter(r,0x111,output);
        CHECK(r.control->stop==StartupStop::InvalidMemory && !r.control->system_parameter.output_written);
        CHECK(r.control->system_parameter.output_write_attempts==0 && !r.control->system_parameter.has_result);
        check_parameter_output(r,0xa5a5a5a5);
    }
}
}
int main() {
    five_real_query_shapes();null_invalid_and_unimplemented();sdk_selectors_and_explicit_projection();
    unavailable_and_invalid_native_values();retained_effects_and_scope();
    std::printf("SYSTEM_PARAMETER_HLE_FAILURES=%d\n",failures);return failures?1:0;
}
